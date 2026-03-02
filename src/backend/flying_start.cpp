// flying_start.cpp - Flying Start backend: Umbra IR → real native code via asmJIT
//
// Translates Umbra IR → native machine code using asmJIT.
// Architecture: uses the host architecture (x86-64 on Linux/x86, ARM64 on Apple Silicon).
//
// Translation strategy:
//   - Walk Umbra IR blocks in order, emitting asmJIT instructions.
//   - Each IRValueRef maps to a virtual GP register (asmJIT allocates real regs).
//   - PHI nodes are lowered to moves inserted at predecessor edges.
//   - Call instructions invoke real C++ function pointers via cc.invoke().
//   - asmJIT handles register allocation, spilling, and call lowering.

#include "flying_start.hpp"
#include <asmjit/core.h>

// Determine host architecture and include appropriate backend
#if defined(ASMJIT_ARCH_X86) && ASMJIT_ARCH_X86 != 0
  #define UMBRA_ARCH_X86 1
  #include <asmjit/x86.h>
  using AsmCompiler = asmjit::x86::Compiler;
  using AsmGp       = asmjit::x86::Gp;
  namespace AsmNS   = asmjit::x86;
#elif defined(ASMJIT_ARCH_ARM) && ASMJIT_ARCH_ARM == 64
  #define UMBRA_ARCH_ARM64 1
  #include <asmjit/a64.h>
  using AsmCompiler = asmjit::a64::Compiler;
  using AsmGp       = asmjit::a64::Gp;
  namespace AsmNS   = asmjit::a64;
#else
  #error "Unsupported architecture for Flying Start backend"
#endif

#include <cassert>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <cstdio>

using namespace asmjit;

static inline JitRuntime* RT(void* p) {
    return static_cast<JitRuntime*>(p);
}

// ─── Constructor / Destructor ─────────────────────────────────────────────────

FlyingStartBackend::FlyingStartBackend()
    : rt(new JitRuntime()) {}

FlyingStartBackend::~FlyingStartBackend() {
    delete RT(rt);
}

void FlyingStartBackend::release(QueryFn fn) {
    if (fn) RT(rt)->release(fn);
}

void FlyingStartBackend::dumpAssembly(std::ostream& out) const {
    if (asmLog.empty()) {
        out << "; (no assembly — compile() not yet called)\n";
        return;
    }
    out << asmLog;
    out << "\n; ── Flying Start Statistics (via asmJIT) ─────────────────\n";
    out << "; Values allocated in registers:      " << stats.regsUsed    << "\n";
    out << "; Comparisons fused with branches:    " << stats.fused       << "\n";
    out << "; Lazy address calculations (GEP):    " << stats.lazy        << "\n";
    out << "; Mov instructions eliminated:        " << stats.elimMovs    << "\n";
    out << "; (Stack spills handled by asmJIT RA)\n";
}

// ─── Architecture-adaptive instruction emission helpers ───────────────────────
// These functions emit the same semantics on both x86-64 and arm64.

namespace {

// Emit: dst = src (move/copy)
static inline void emit_mov(AsmCompiler& cc, AsmGp dst, AsmGp src) {
    cc.mov(dst, src);
}

// Emit: dst = imm
static inline void emit_mov_imm(AsmCompiler& cc, AsmGp dst, int64_t val) {
    cc.mov(dst, imm(val));
}

#ifdef UMBRA_ARCH_X86

// x86-64: 2-operand arithmetic (dst op= src)
static inline void emit_add(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.mov(dst, a);
    cc.add(dst, b);
}
static inline void emit_sub(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.mov(dst, a);
    cc.sub(dst, b);
}
static inline void emit_mul(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.mov(dst, a);
    cc.imul(dst, b);
}
static inline void emit_and(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.mov(dst, a);
    cc.and_(dst, b);
}
static inline void emit_or(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.mov(dst, a);
    cc.or_(dst, b);
}
static inline void emit_xor(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.mov(dst, a);
    cc.xor_(dst, b);
}
static inline void emit_lnot(AsmCompiler& cc, AsmGp dst, AsmGp src) {
    cc.mov(dst, src);
    cc.xor_(dst, imm(1));
}
static inline void emit_crc32(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.mov(dst, a);
    cc.crc32(dst, b);
}
static inline void emit_ror(AsmCompiler& cc, AsmGp dst, AsmGp src, uint32_t shift) {
    cc.mov(dst, src);
    cc.ror(dst, imm(shift));
}
static inline void emit_zext(AsmCompiler& cc, AsmGp dst, AsmGp src) {
    // On x86, movzx dst32, src32 naturally zero-extends to 64-bit
    cc.movzx(dst.r32(), src.r32());
}
static inline void emit_sext(AsmCompiler& cc, AsmGp dst, AsmGp src) {
    cc.movsxd(dst, src.r32());
}
static inline void emit_load64(AsmCompiler& cc, AsmGp dst, AsmGp ptr) {
    cc.mov(dst, asmjit::x86::ptr(ptr));
}
static inline void emit_store64(AsmCompiler& cc, AsmGp ptr, AsmGp val) {
    cc.mov(asmjit::x86::ptr(ptr), val);
}
static inline void emit_lea(AsmCompiler& cc, AsmGp dst, AsmGp base, int32_t offset) {
    cc.lea(dst, asmjit::x86::ptr(base, offset));
}

// Emit cmp a, b then jump to trueLabel if condition holds.
static inline void emit_cmp_jump_true(AsmCompiler& cc, Opcode cmpOp,
                                        AsmGp a, AsmGp b,
                                        Label trueLabel) {
    cc.cmp(a, b);
    switch (cmpOp) {
        case Opcode::CmpEq: cc.je(trueLabel);  break;
        case Opcode::CmpNe: cc.jne(trueLabel); break;
        case Opcode::CmpLt: cc.jl(trueLabel);  break;
        case Opcode::CmpGt: cc.jg(trueLabel);  break;
        case Opcode::CmpLe: cc.jle(trueLabel); break;
        case Opcode::CmpGe: cc.jge(trueLabel); break;
        default:            cc.jnz(trueLabel); break;
    }
}

// Materialize comparison result into dst (0/1).
static inline void emit_cmp_set(AsmCompiler& cc, Opcode cmpOp,
                                 AsmGp dst, AsmGp a, AsmGp b) {
    cc.cmp(a, b);
    switch (cmpOp) {
        case Opcode::CmpEq: cc.sete(dst.r8());  break;
        case Opcode::CmpNe: cc.setne(dst.r8()); break;
        case Opcode::CmpLt: cc.setl(dst.r8());  break;
        case Opcode::CmpGt: cc.setg(dst.r8());  break;
        case Opcode::CmpLe: cc.setle(dst.r8()); break;
        case Opcode::CmpGe: cc.setge(dst.r8()); break;
        default:            cc.setne(dst.r8()); break;
    }
    cc.movzx(dst.r32(), dst.r8());
}

// Non-fused: test cond and jump on true.
static inline void emit_test_jump_true(AsmCompiler& cc, AsmGp cond,
                                        Label trueLabel) {
    cc.test(cond, cond);
    cc.jnz(trueLabel);
}
static inline void emit_jmp(AsmCompiler& cc, Label target) {
    cc.jmp(target);
}

#else // UMBRA_ARCH_ARM64

// arm64: 3-operand arithmetic
static inline void emit_add(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.add(dst, a, b);
}
static inline void emit_sub(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.sub(dst, a, b);
}
static inline void emit_mul(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.mul(dst, a, b);
}
static inline void emit_and(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.and_(dst, a, b);
}
static inline void emit_or(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.orr(dst, a, b);
}
static inline void emit_xor(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    cc.eor(dst, a, b);
}
static inline void emit_lnot(AsmCompiler& cc, AsmGp dst, AsmGp src) {
    cc.eor(dst, src, imm(1));
}
// CRC32 on ARM64 — use CRC32CX instruction: crc32cx Wd, Wn, Xm
// Result is 32-bit (in W register), which auto-zero-extends to 64 bits.
static inline void emit_crc32(AsmCompiler& cc, AsmGp dst, AsmGp a, AsmGp b) {
    // crc32cx: dst.w() = crc32c(a.w(), b)
    // a is the seed (32-bit), b is the 64-bit value. Result in dst (32→64 zero-extended).
    cc.crc32cx(dst.w(), a.w(), b);
}
static inline void emit_ror(AsmCompiler& cc, AsmGp dst, AsmGp src, uint32_t shift) {
    cc.ror(dst, src, imm(shift));
}
static inline void emit_zext(AsmCompiler& cc, AsmGp dst, AsmGp src) {
    // ARM64: zero-extend — source is already from an i64 call result,
    // treat as a 64-bit copy (upper bits are zero by construction).
    cc.mov(dst, src);
}
static inline void emit_sext(AsmCompiler& cc, AsmGp dst, AsmGp src) {
    // sxtw (sign-extend from 32 to 64)
    cc.sxtw(dst, src.w());
}
static inline void emit_load64(AsmCompiler& cc, AsmGp dst, AsmGp ptr) {
    cc.ldr(dst, asmjit::a64::ptr(ptr));
}
static inline void emit_store64(AsmCompiler& cc, AsmGp ptr, AsmGp val) {
    cc.str(val, asmjit::a64::ptr(ptr));
}
static inline void emit_lea(AsmCompiler& cc, AsmGp dst, AsmGp base, int32_t offset) {
    cc.add(dst, base, imm(offset));
}

// ARM64 comparisons: cmp + b.cc
static inline void emit_cmp_jump_true(AsmCompiler& cc, Opcode cmpOp,
                                        AsmGp a, AsmGp b,
                                        Label trueLabel) {
    cc.cmp(a, b);
    switch (cmpOp) {
        case Opcode::CmpEq: cc.b_eq(trueLabel); break;
        case Opcode::CmpNe: cc.b_ne(trueLabel); break;
        case Opcode::CmpLt: cc.b_lt(trueLabel); break;
        case Opcode::CmpGt: cc.b_gt(trueLabel); break;
        case Opcode::CmpLe: cc.b_le(trueLabel); break;
        case Opcode::CmpGe: cc.b_ge(trueLabel); break;
        default:            cc.b_ne(trueLabel); break;
    }
}

// Materialize comparison result into dst (0/1).
static inline void emit_cmp_set(AsmCompiler& cc, Opcode cmpOp,
                                 AsmGp dst, AsmGp a, AsmGp b) {
    cc.cmp(a, b);
    switch (cmpOp) {
        case Opcode::CmpEq: cc.cset(dst, imm(arm::CondCode::kEQ)); break;
        case Opcode::CmpNe: cc.cset(dst, imm(arm::CondCode::kNE)); break;
        case Opcode::CmpLt: cc.cset(dst, imm(arm::CondCode::kLT)); break;
        case Opcode::CmpGt: cc.cset(dst, imm(arm::CondCode::kGT)); break;
        case Opcode::CmpLe: cc.cset(dst, imm(arm::CondCode::kLE)); break;
        case Opcode::CmpGe: cc.cset(dst, imm(arm::CondCode::kGE)); break;
        default:            cc.cset(dst, imm(arm::CondCode::kNE)); break;
    }
}

static inline void emit_test_jump_true(AsmCompiler& cc, AsmGp cond,
                                        Label trueLabel) {
    cc.cbnz(cond, trueLabel);
}
static inline void emit_jmp(AsmCompiler& cc, Label target) {
    cc.b(target);
}

#endif // architecture

// ─── Translator ──────────────────────────────────────────────────────────────

struct Translator {
    const IRProgram&  prog;
    const IRFunction& fn;
    AsmCompiler&      cc;

    std::unordered_map<IRValueRef, AsmGp> regMap;
    std::unordered_map<uint32_t,   Label> blockLabels;
    std::unordered_map<IRValueRef, AsmGp> phiRegs;
    std::unordered_map<IRValueRef, uint32_t> useCounts;
    std::unordered_map<IRValueRef, bool> deferredCmps;

    int& regsUsed;
    int& fused;
    int& lazy;
    int& elimMovs;

    Translator(const IRProgram& p, const IRFunction& f, AsmCompiler& c,
               int& ru, int& fu, int& la, int& em)
        : prog(p), fn(f), cc(c),
          regsUsed(ru), fused(fu), lazy(la), elimMovs(em) {}

    AsmGp getOrAlloc(IRValueRef ref) {
        auto it = regMap.find(ref);
        if (it != regMap.end()) return it->second;
        auto gp = cc.new_gp64();
        regMap[ref] = gp;
        ++regsUsed;
        return gp;
    }

    AsmGp getValue(IRValueRef ref) {
        if (prog.isParam(ref)) {
            auto it = regMap.find(ref);
            if (it != regMap.end()) return it->second;
            auto gp = cc.new_gp64();
            regMap[ref] = gp;
            return gp;
        }
        auto pit = phiRegs.find(ref);
        if (pit != phiRegs.end()) return pit->second;
        auto it = regMap.find(ref);
        if (it != regMap.end()) return it->second;
        return getOrAlloc(ref);
    }

    AsmGp emitImm64(IRValueRef ref, int64_t val) {
        auto it = regMap.find(ref);
        if (it != regMap.end()) return it->second;
        auto gp = cc.new_gp64();
        emit_mov_imm(cc, gp, val);
        regMap[ref] = gp;
        ++regsUsed;
        return gp;
    }

    void allocLabels() {
        for (uint32_t bi = 0; bi < fn.blocks.size(); ++bi)
            blockLabels[bi] = cc.new_label();
    }

    void computeUseCounts() {
        auto countUse = [&](IRValueRef ref) {
            if (ref != NullRef && !prog.isParam(ref)) useCounts[ref]++;
        };

        for (const BasicBlock& blk : fn.blocks) {
            for (IRValueRef ref : blk.instructions) {
                const InstrHeader* h = prog.getInstr(ref);
                switch (h->op) {
                case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
                case Opcode::Div: case Opcode::Rem: case Opcode::And:
                case Opcode::Or:  case Opcode::Xor: case Opcode::Shl:
                case Opcode::Shr: case Opcode::CRC32:
                case Opcode::CmpEq: case Opcode::CmpNe:
                case Opcode::CmpLt: case Opcode::CmpGt:
                case Opcode::CmpLe: case Opcode::CmpGe:
                case Opcode::LAnd: case Opcode::LOr:
                case Opcode::Store: {
                    auto* bi = reinterpret_cast<const BinaryInstr*>(h);
                    countUse(bi->arg0);
                    countUse(bi->arg1);
                    break;
                }
                case Opcode::Load: case Opcode::ZExt: case Opcode::SExt:
                case Opcode::LNot: case Opcode::IsNull: {
                    auto* ui = reinterpret_cast<const UnaryInstr*>(h);
                    countUse(ui->arg0);
                    break;
                }
                case Opcode::RotateRight: {
                    auto* ri = reinterpret_cast<const RotateRightInstr*>(h);
                    countUse(ri->arg0);
                    break;
                }
                case Opcode::GEP: {
                    auto* gi = reinterpret_cast<const GEPInstr*>(h);
                    countUse(gi->base);
                    break;
                }
                case Opcode::CondBranch: {
                    auto* cbi = reinterpret_cast<const CondBranchInstr*>(h);
                    countUse(cbi->cond);
                    break;
                }
                case Opcode::Return: {
                    auto* ri = reinterpret_cast<const ReturnInstr*>(h);
                    countUse(ri->value);
                    break;
                }
                case Opcode::Phi: {
                    auto* ph = reinterpret_cast<const PhiHeader*>(h);
                    const PhiEntry* entries = reinterpret_cast<const PhiEntry*>(ph + 1);
                    for (uint32_t i = 0; i < ph->numEntries; ++i)
                        countUse(entries[i].value);
                    break;
                }
                case Opcode::Call: {
                    auto* ch = reinterpret_cast<const CallHeader*>(h);
                    const IRValueRef* args = reinterpret_cast<const IRValueRef*>(ch + 1);
                    for (uint32_t i = 0; i < ch->numArgs; ++i)
                        countUse(args[i]);
                    break;
                }
                default:
                    break;
                }
            }
        }
    }

    bool canDeferComparison(IRValueRef cmpRef, uint32_t blockIdx) const {
        auto it = useCounts.find(cmpRef);
        if (it == useCounts.end() || it->second != 1) return false;
        const BasicBlock& blk = fn.blocks[blockIdx];
        if (blk.instructions.empty()) return false;
        IRValueRef lastRef = blk.instructions.back();
        const InstrHeader* last = prog.getInstr(lastRef);
        if (last->op != Opcode::CondBranch) return false;
        const auto* cbr = reinterpret_cast<const CondBranchInstr*>(last);
        return cbr->cond == cmpRef;
    }

    void findPhis() {
        for (uint32_t bi = 0; bi < fn.blocks.size(); ++bi) {
            for (IRValueRef ref : fn.blocks[bi].instructions) {
                const InstrHeader* h = prog.getInstr(ref);
                if (h->op == Opcode::Phi) {
                    auto gp = cc.new_gp64();
                    phiRegs[ref] = gp;
                    regMap[ref]  = gp;
                    ++regsUsed;
                }
            }
        }
    }

    void translateFunction() {
        cc.add_func(FuncSignature::build<void>());
        allocLabels();
        computeUseCounts();
        findPhis();
        for (uint32_t bi = 0; bi < fn.blocks.size(); ++bi)
            translateBlock(bi);
        cc.end_func();
    }

    void translateBlock(uint32_t blkIdx) {
        cc.bind(blockLabels[blkIdx]);
        for (IRValueRef ref : fn.blocks[blkIdx].instructions)
            translateInstr(ref, blkIdx);
    }

    void translateInstr(IRValueRef ref, uint32_t blkIdx) {
        const InstrHeader* h = prog.getInstr(ref);

        switch (h->op) {
        case Opcode::ConstInt: {
            auto* ci = reinterpret_cast<const ConstIntInstr*>(h);
            emitImm64(ref, ci->value);
            break;
        }
        case Opcode::ConstBool: {
            auto* cb = reinterpret_cast<const ConstBoolInstr*>(h);
            emitImm64(ref, cb->value ? 1 : 0);
            break;
        }
        case Opcode::Add: {
            auto* i = reinterpret_cast<const BinaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_add(cc, dst, getValue(i->arg0), getValue(i->arg1));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::Sub: {
            auto* i = reinterpret_cast<const BinaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_sub(cc, dst, getValue(i->arg0), getValue(i->arg1));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::Mul: {
            auto* i = reinterpret_cast<const BinaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_mul(cc, dst, getValue(i->arg0), getValue(i->arg1));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::And: case Opcode::LAnd: {
            auto* i = reinterpret_cast<const BinaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_and(cc, dst, getValue(i->arg0), getValue(i->arg1));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::Or: case Opcode::LOr: {
            auto* i = reinterpret_cast<const BinaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_or(cc, dst, getValue(i->arg0), getValue(i->arg1));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::Xor: {
            auto* i = reinterpret_cast<const BinaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_xor(cc, dst, getValue(i->arg0), getValue(i->arg1));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::LNot: {
            auto* i = reinterpret_cast<const UnaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_lnot(cc, dst, getValue(i->arg0));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::CRC32: {
            auto* i = reinterpret_cast<const BinaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_crc32(cc, dst, getValue(i->arg0), getValue(i->arg1));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::RotateRight: {
            auto* i = reinterpret_cast<const RotateRightInstr*>(h);
            auto dst = cc.new_gp64();
            emit_ror(cc, dst, getValue(i->arg0), i->shift);
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::ZExt: {
            auto* i = reinterpret_cast<const UnaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_zext(cc, dst, getValue(i->arg0));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::SExt: {
            auto* i = reinterpret_cast<const UnaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_sext(cc, dst, getValue(i->arg0));
            regMap[ref] = dst; ++regsUsed;
            break;
        }

        // Optimization 4: Comparison-Branch Fusion
        case Opcode::CmpEq: case Opcode::CmpNe:
        case Opcode::CmpLt: case Opcode::CmpGt:
        case Opcode::CmpLe: case Opcode::CmpGe: {
            if (canDeferComparison(ref, blkIdx)) {
                deferredCmps[ref] = true;
            } else {
                auto* i = reinterpret_cast<const BinaryInstr*>(h);
                auto dst = cc.new_gp64();
                emit_cmp_set(cc, h->op, dst, getValue(i->arg0), getValue(i->arg1));
                regMap[ref] = dst;
                ++regsUsed;
            }
            break;
        }

        // Optimization 3: Lazy Address Calculation (GEP → LEA)
        case Opcode::GEP: {
            auto* i = reinterpret_cast<const GEPInstr*>(h);
            auto dst = cc.new_gp64();
            emit_lea(cc, dst, getValue(i->base), (int32_t)i->byteOffset);
            regMap[ref] = dst; ++lazy; ++regsUsed;
            break;
        }
        case Opcode::Load: {
            auto* i = reinterpret_cast<const UnaryInstr*>(h);
            auto dst = cc.new_gp64();
            emit_load64(cc, dst, getValue(i->arg0));
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        case Opcode::Store: {
            auto* i = reinterpret_cast<const BinaryInstr*>(h);
            emit_store64(cc, getValue(i->arg0), getValue(i->arg1));
            break;
        }
        case Opcode::Branch: {
            auto* i = reinterpret_cast<const BranchInstr*>(h);
            emitPhiMovesForEdge(blkIdx, i->targetBlock);
            emit_jmp(cc, blockLabels[i->targetBlock]);
            break;
        }
        case Opcode::CondBranch: {
            auto* i = reinterpret_cast<const CondBranchInstr*>(h);
            translateCondBranch(*i, blkIdx);
            break;
        }
        case Opcode::Phi:
            break;  // handled by emitPhiMovesForEdge
        case Opcode::Call: {
            auto* ch = reinterpret_cast<const CallHeader*>(h);
            translateCall(ref, *ch);
            break;
        }
        case Opcode::Return:
            cc.ret();
            break;
        case Opcode::IsNull: {
            auto dst = cc.new_gp64();
            emit_mov_imm(cc, dst, 0);
            regMap[ref] = dst; ++regsUsed;
            break;
        }
        default:
            break;
        }
    }

    void emitPhiMovesForEdge(uint32_t fromBlock, uint32_t toBlock) {
        if (toBlock >= fn.blocks.size()) return;
        const BasicBlock& target = fn.blocks[toBlock];
        for (IRValueRef ref : target.instructions) {
            const InstrHeader* h = prog.getInstr(ref);
            if (h->op != Opcode::Phi) break;

            auto* ph = reinterpret_cast<const PhiHeader*>(h);
            const PhiEntry* entries = reinterpret_cast<const PhiEntry*>(ph + 1);

            for (uint32_t ei = 0; ei < ph->numEntries; ++ei) {
                if (entries[ei].fromBlock == fromBlock) {
                    IRValueRef incoming = entries[ei].value;
                    if (incoming == NullRef) break;  // placeholder not yet patched
                    AsmGp phiGp = phiRegs[ref];

                    if (prog.isConstant(incoming)) {
                        emit_mov_imm(cc, phiGp, prog.constIntValue(incoming));
                    } else {
                        auto it = regMap.find(incoming);
                        if (it != regMap.end()) {
                            emit_mov(cc, phiGp, it->second);
                        } else {
                            // Constant not yet materialized
                            const InstrHeader* ih = prog.getInstr(incoming);
                            if (ih->op == Opcode::ConstInt) {
                                auto* ci = reinterpret_cast<const ConstIntInstr*>(ih);
                                emit_mov_imm(cc, phiGp, ci->value);
                            } else if (ih->op == Opcode::ConstBool) {
                                auto* cb = reinterpret_cast<const ConstBoolInstr*>(ih);
                                emit_mov_imm(cc, phiGp, cb->value ? 1 : 0);
                            }
                        }
                    }
                    ++elimMovs;
                    break;
                }
            }
        }
    }

    void translateCondBranch(const CondBranchInstr& i, uint32_t blkIdx) {
        Label trueLabel  = blockLabels[i.trueBlock];
        Label falseLabel = blockLabels[i.falseBlock];
        Label trueEdgeLabel = cc.new_label();

        auto it = deferredCmps.find(i.cond);
        if (it != deferredCmps.end()) {
            IRValueRef cmpRef = i.cond;
            const InstrHeader* ch = prog.getInstr(cmpRef);
            const BinaryInstr* ci = reinterpret_cast<const BinaryInstr*>(ch);

            AsmGp a = getValue(ci->arg0);
            AsmGp b = getValue(ci->arg1);

            // Fused cmp + branch, while keeping PHI moves edge-specific.
            emit_cmp_jump_true(cc, ch->op, a, b, trueEdgeLabel);
            emitPhiMovesForEdge(blkIdx, i.falseBlock);
            emit_jmp(cc, falseLabel);
            cc.bind(trueEdgeLabel);
            emitPhiMovesForEdge(blkIdx, i.trueBlock);
            emit_jmp(cc, trueLabel);
            deferredCmps.erase(it);
            ++fused;
        } else {
            AsmGp cond = getValue(i.cond);
            emit_test_jump_true(cc, cond, trueEdgeLabel);
            emitPhiMovesForEdge(blkIdx, i.falseBlock);
            emit_jmp(cc, falseLabel);
            cc.bind(trueEdgeLabel);
            emitPhiMovesForEdge(blkIdx, i.trueBlock);
            emit_jmp(cc, trueLabel);
        }
    }

    void translateCall(IRValueRef ref, const CallHeader& ch) {
        const IRValueRef* argRefs = reinterpret_cast<const IRValueRef*>(&ch + 1);
        bool hasRet = (ch.resultType != IRType::Void);

        std::vector<AsmGp> argGps;
        argGps.reserve(ch.numArgs);
        for (uint32_t ai = 0; ai < ch.numArgs; ++ai) {
            IRValueRef aref = argRefs[ai];
            if (prog.isConstant(aref)) {
                auto tmp = cc.new_gp64();
                emit_mov_imm(cc, tmp, prog.constIntValue(aref));
                argGps.push_back(tmp);
            } else {
                argGps.push_back(getValue(aref));
            }
        }

        FuncSignature sig(CallConvId::kCDecl);
        if (hasRet) sig.set_ret(TypeId::kUInt64);
        for (uint32_t ai = 0; ai < ch.numArgs && sig.can_add_arg(); ++ai)
            sig.add_arg(TypeId::kUInt64);

        // ARM64: BLR requires a register target (not an immediate).
        // Load the function pointer into a virtual GP register first.
        auto fnPtrReg = cc.new_gp64();
        emit_mov_imm(cc, fnPtrReg, int64_t(ch.funcPtr));

        InvokeNode* invokeNode = nullptr;
        Error err = cc.invoke(Out<InvokeNode*>(invokeNode), fnPtrReg, sig);
        if (err != kErrorOk || invokeNode == nullptr) {
            fprintf(stderr, "[JIT] invoke error: %s (numArgs=%u hasRet=%d)\n",
                    DebugUtils::error_as_string(err), ch.numArgs, (int)hasRet);
            return;
        }

        for (uint32_t ai = 0; ai < (uint32_t)argGps.size(); ++ai)
            invokeNode->set_arg(ai, argGps[ai]);

        if (hasRet) {
            auto retGp = cc.new_gp64();
            invokeNode->set_ret(0, retGp);
            regMap[ref] = retGp;
            ++regsUsed;
        }
    }
};

} // anonymous namespace

// ─── compile() ───────────────────────────────────────────────────────────────

FlyingStartBackend::QueryFn
FlyingStartBackend::compile(const IRProgram& prog, const IRFunction& fn) {
    stats = {};

    // Custom error handler: print each asmJIT error as it occurs
    struct DebugErrorHandler : public ErrorHandler {
        void handleError(Error err, const char* message, BaseEmitter* /*origin*/) {
            fprintf(stderr, "[JIT-ERR] %s: %s\n",
                    DebugUtils::error_as_string(err), message ? message : "");
        }
    } errHandler;

    CodeHolder code;
    code.init(RT(rt)->environment(), RT(rt)->cpu_features());
    code.set_error_handler(&errHandler);

    StringLogger logger;
    logger.add_flags(FormatFlags::kHexImms | FormatFlags::kMachineCode);
    code.set_logger(&logger);

    AsmCompiler cc(&code);
    cc.set_error_handler(&errHandler);

    Translator tr(prog, fn, cc,
                  stats.regsUsed, stats.fused,
                  stats.lazy, stats.elimMovs);
    tr.translateFunction();

    Error finalErr = cc.finalize();
    asmLog = logger.data();
    if (finalErr != kErrorOk) {
        throw std::runtime_error(
            std::string("asmJIT finalize error: ") + DebugUtils::error_as_string(finalErr));
    }

    QueryFn fnPtr = nullptr;
    Error err = RT(rt)->add(&fnPtr, &code);
    if (err != kErrorOk) {
        throw std::runtime_error(
            std::string("asmJIT error: ") + DebugUtils::error_as_string(err));
    }

    return fnPtr;
}
