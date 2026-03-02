// umbra_ir.cpp - IRProgram implementation
#include "umbra_ir.hpp"
#include <stdexcept>
#include <algorithm>

// ─── Function / block management ──────────────────────────────────────────────

uint32_t IRProgram::beginFunction(const std::string& name,
                                  IRType returnType,
                                  std::vector<std::pair<std::string,IRType>> params) {
    uint32_t fnIdx = static_cast<uint32_t>(functions.size());
    functions.push_back({});
    IRFunction& fn = functions.back();
    fn.name       = name;
    fn.returnType = returnType;

    for (uint32_t i = 0; i < params.size(); ++i) {
        fn.paramNames.push_back(params[i].first);
        fn.paramTypes.push_back(params[i].second);
        fn.paramRefs.push_back(paramRef(fnIdx, i));
    }

    // Add entry block automatically
    fn.blocks.push_back({"entry", {}});

    currentFnIdx  = fnIdx;
    currentBlkIdx = 0;
    return fnIdx;
}

uint32_t IRProgram::addBlock(const std::string& name) {
    IRFunction& fn = functions[currentFnIdx];
    uint32_t idx = static_cast<uint32_t>(fn.blocks.size());
    fn.blocks.push_back({name, {}});
    return idx;
}

void IRProgram::setInsertionPoint(uint32_t fnIdx, uint32_t blkIdx) {
    currentFnIdx  = fnIdx;
    currentBlkIdx = blkIdx;
}

// ─── Instruction builders ──────────────────────────────────────────────────────

IRValueRef IRProgram::addConstInt(int64_t value, IRType t) {
    // Deduplicate constants
    uint64_t key = (uint64_t(static_cast<uint8_t>(t)) << 56) ^
                   (uint64_t)(uint64_t)value;
    auto it = constIntCache.find(key);
    if (it != constIntCache.end()) return it->second;

    ConstIntInstr instr{Opcode::ConstInt, t, value};
    IRValueRef ref = emit(instr);
    constIntCache[key] = ref;
    return ref;
}

IRValueRef IRProgram::addConstBool(bool value) {
    ConstBoolInstr instr{Opcode::ConstBool, IRType::Bool, value};
    return emit(instr);
}

IRValueRef IRProgram::addBinary(Opcode op, IRType resultType,
                                 IRValueRef a, IRValueRef b) {
    // Try constant folding first
    IRValueRef folded = tryConstFold(op, resultType, a, b);
    if (folded != NullRef) return folded;

    BinaryInstr instr{op, resultType, a, b};
    return emit(instr);
}

IRValueRef IRProgram::addUnary(Opcode op, IRType resultType, IRValueRef a) {
    UnaryInstr instr{op, resultType, a};
    return emit(instr);
}

IRValueRef IRProgram::addRotateRight(IRValueRef val, uint32_t shift) {
    RotateRightInstr instr{Opcode::RotateRight, IRType::UInt64, val, shift};
    return emit(instr);
}

IRValueRef IRProgram::addGEP(IRValueRef base, int64_t byteOffset) {
    if (byteOffset == 0) return base;  // trivial fold
    GEPInstr instr{Opcode::GEP, IRType::Ptr, base, byteOffset};
    return emit(instr);
}

IRValueRef IRProgram::addLoad(IRType resultType, IRValueRef ptr) {
    UnaryInstr instr{Opcode::Load, resultType, ptr};
    return emit(instr);
}

IRValueRef IRProgram::addStore(IRValueRef ptr, IRValueRef value) {
    BinaryInstr instr{Opcode::Store, IRType::Void, ptr, value};
    return emit(instr);
}

IRValueRef IRProgram::addBranch(uint32_t targetBlock) {
    BranchInstr instr{Opcode::Branch, IRType::Void, targetBlock};
    return emit(instr);
}

IRValueRef IRProgram::addCondBranch(IRValueRef cond,
                                     uint32_t trueBlock, uint32_t falseBlock) {
    CondBranchInstr instr{Opcode::CondBranch, IRType::Void,
                           cond, trueBlock, falseBlock};
    return emit(instr);
}

IRValueRef IRProgram::addReturn(IRValueRef value) {
    ReturnInstr instr{Opcode::Return, IRType::Void, value};
    return emit(instr);
}

IRValueRef IRProgram::addPhi(IRType resultType,
                              std::vector<std::pair<IRValueRef,uint32_t>> incoming) {
    // Write PhiHeader then N PhiEntry records directly to instrData
    IRValueRef offset = static_cast<IRValueRef>(instrData.size());
    PhiHeader hdr{Opcode::Phi, resultType,
                  static_cast<uint32_t>(incoming.size())};
    size_t totalSize = sizeof(PhiHeader) + incoming.size() * sizeof(PhiEntry);
    instrData.resize(instrData.size() + totalSize);
    uint8_t* ptr = instrData.data() + offset;
    std::memcpy(ptr, &hdr, sizeof(PhiHeader));
    ptr += sizeof(PhiHeader);
    for (auto& [val, blk] : incoming) {
        PhiEntry entry{val, blk};
        std::memcpy(ptr, &entry, sizeof(PhiEntry));
        ptr += sizeof(PhiEntry);
    }
    currentBlock().instructions.push_back(offset);
    return offset;
}

void IRProgram::patchPhiEntry(IRValueRef phiRef, uint32_t entryIdx,
                               IRValueRef newValue, uint32_t fromBlock) {
    PhiHeader* ph = reinterpret_cast<PhiHeader*>(instrData.data() + phiRef);
    assert(ph->op == Opcode::Phi);
    assert(entryIdx < ph->numEntries);
    PhiEntry* entries = reinterpret_cast<PhiEntry*>(ph + 1);
    entries[entryIdx].value     = newValue;
    entries[entryIdx].fromBlock = fromBlock;
}

IRValueRef IRProgram::addCall(IRType resultType,
                               uint64_t funcPtr,
                               std::vector<IRValueRef> args) {
    IRValueRef offset = static_cast<IRValueRef>(instrData.size());
    CallHeader hdr{Opcode::Call, resultType, funcPtr,
                   static_cast<uint32_t>(args.size())};
    size_t totalSize = sizeof(CallHeader) + args.size() * sizeof(IRValueRef);
    instrData.resize(instrData.size() + totalSize);
    uint8_t* ptr = instrData.data() + offset;
    std::memcpy(ptr, &hdr, sizeof(CallHeader));
    ptr += sizeof(CallHeader);
    for (IRValueRef arg : args) {
        std::memcpy(ptr, &arg, sizeof(IRValueRef));
        ptr += sizeof(IRValueRef);
    }
    currentBlock().instructions.push_back(offset);
    return offset;
}

// ─── Instruction size ──────────────────────────────────────────────────────────

size_t IRProgram::instrSize(IRValueRef ref) const {
    const InstrHeader* h = getInstr(ref);
    switch (h->op) {
        case Opcode::ConstInt:     return sizeof(ConstIntInstr);
        case Opcode::ConstBool:    return sizeof(ConstBoolInstr);
        case Opcode::RotateRight:  return sizeof(RotateRightInstr);
        case Opcode::GEP:          return sizeof(GEPInstr);
        case Opcode::Branch:       return sizeof(BranchInstr);
        case Opcode::CondBranch:   return sizeof(CondBranchInstr);
        case Opcode::Return:       return sizeof(ReturnInstr);
        case Opcode::Phi: {
            const PhiHeader* ph = reinterpret_cast<const PhiHeader*>(h);
            return sizeof(PhiHeader) + ph->numEntries * sizeof(PhiEntry);
        }
        case Opcode::Call: {
            const CallHeader* ch = reinterpret_cast<const CallHeader*>(h);
            return sizeof(CallHeader) + ch->numArgs * sizeof(IRValueRef);
        }
        // Binary instructions
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
        case Opcode::Div: case Opcode::Rem: case Opcode::And:
        case Opcode::Or:  case Opcode::Xor: case Opcode::Shl:
        case Opcode::Shr: case Opcode::CRC32:
        case Opcode::CmpEq: case Opcode::CmpNe:
        case Opcode::CmpLt: case Opcode::CmpGt:
        case Opcode::CmpLe: case Opcode::CmpGe:
        case Opcode::LAnd: case Opcode::LOr:
        case Opcode::Store:
            return sizeof(BinaryInstr);
        // Unary instructions
        case Opcode::Load: case Opcode::ZExt: case Opcode::SExt:
        case Opcode::LNot: case Opcode::IsNull:
            return sizeof(UnaryInstr);
    }
    return sizeof(InstrHeader);  // fallback
}

// ─── Type and constant helpers ─────────────────────────────────────────────────

IRType IRProgram::typeOf(IRValueRef ref) const {
    if (isParam(ref)) {
        uint32_t fnIdx    = (ref >> 16) & 0xFFu;
        uint32_t paramIdx = ref & 0xFFFFu;
        return functions[fnIdx].paramTypes[paramIdx];
    }
    return getInstr(ref)->resultType;
}

bool IRProgram::isConstant(IRValueRef ref) const {
    if (isParam(ref)) return false;
    auto op = getInstr(ref)->op;
    return op == Opcode::ConstInt || op == Opcode::ConstBool;
}

int64_t IRProgram::constIntValue(IRValueRef ref) const {
    const InstrHeader* h = getInstr(ref);
    if (h->op == Opcode::ConstInt) {
        return reinterpret_cast<const ConstIntInstr*>(h)->value;
    }
    if (h->op == Opcode::ConstBool) {
        return reinterpret_cast<const ConstBoolInstr*>(h)->value ? 1 : 0;
    }
    throw std::runtime_error("constIntValue called for non-constant");
}

// ─── Constant folding ──────────────────────────────────────────────────────────

IRValueRef IRProgram::tryConstFold(Opcode op, IRType t,
                                    IRValueRef a, IRValueRef b) {
    if (!isConstant(a) || !isConstant(b)) return NullRef;

    int64_t va = constIntValue(a);
    int64_t vb = constIntValue(b);
    int64_t result = 0;
    IRType  rtype  = t;

    switch (op) {
        case Opcode::Add:   result = va + vb; break;
        case Opcode::Sub:   result = va - vb; break;
        case Opcode::Mul:   result = va * vb; break;
        case Opcode::Div:   if (vb == 0) return NullRef; result = va / vb; break;
        case Opcode::CmpEq: result = va == vb; rtype = IRType::Bool; break;
        case Opcode::CmpNe: result = va != vb; rtype = IRType::Bool; break;
        case Opcode::CmpLt: result = va <  vb; rtype = IRType::Bool; break;
        case Opcode::CmpGt: result = va >  vb; rtype = IRType::Bool; break;
        case Opcode::CmpLe: result = va <= vb; rtype = IRType::Bool; break;
        case Opcode::CmpGe: result = va >= vb; rtype = IRType::Bool; break;
        case Opcode::And:   result = va & vb;  break;
        case Opcode::Or:    result = va | vb;  break;
        case Opcode::Xor:   result = va ^ vb;  break;
        case Opcode::LAnd:  result = (va != 0) && (vb != 0); rtype = IRType::Bool; break;
        case Opcode::LOr:   result = (va != 0) || (vb != 0); rtype = IRType::Bool; break;
        default:            return NullRef;
    }

    if (rtype == IRType::Bool) {
        return addConstBool(result != 0);
    }
    return addConstInt(result, rtype);
}

// ─── Dead code elimination ─────────────────────────────────────────────────────

void IRProgram::eliminateDeadCode() {
    // Remove instructions in unreachable blocks first.
    for (auto& fn : functions) {
        if (fn.blocks.empty()) continue;

        std::vector<uint8_t> reachable(fn.blocks.size(), 0);
        std::vector<uint32_t> worklist;
        worklist.push_back(0);

        while (!worklist.empty()) {
            uint32_t bi = worklist.back();
            worklist.pop_back();
            if (bi >= fn.blocks.size() || reachable[bi]) continue;
            reachable[bi] = 1;

            const BasicBlock& blk = fn.blocks[bi];
            if (blk.instructions.empty()) continue;

            const InstrHeader* term = getInstr(blk.instructions.back());
            if (term->op == Opcode::Branch) {
                const auto* br = reinterpret_cast<const BranchInstr*>(term);
                worklist.push_back(br->targetBlock);
            } else if (term->op == Opcode::CondBranch) {
                const auto* cbr = reinterpret_cast<const CondBranchInstr*>(term);
                worklist.push_back(cbr->trueBlock);
                worklist.push_back(cbr->falseBlock);
            }
        }

        for (uint32_t bi = 0; bi < fn.blocks.size(); ++bi) {
            if (!reachable[bi]) {
                fn.blocks[bi].instructions.clear();
            }
        }
    }

    // Count uses of every IRValueRef
    std::unordered_map<IRValueRef, int> useCounts;

    auto countUse = [&](IRValueRef ref) {
        if (ref != NullRef && !isParam(ref))
            useCounts[ref]++;
    };

    for (auto& fn : functions) {
        for (auto& blk : fn.blocks) {
            for (IRValueRef ref : blk.instructions) {
                const InstrHeader* h = getInstr(ref);
                switch (h->op) {
                    case Opcode::Add: case Opcode::Sub: case Opcode::Mul:
                    case Opcode::Div: case Opcode::Rem: case Opcode::And:
                    case Opcode::Or:  case Opcode::Xor: case Opcode::Shl:
                    case Opcode::Shr: case Opcode::CRC32:
                    case Opcode::CmpEq: case Opcode::CmpNe:
                    case Opcode::CmpLt: case Opcode::CmpGt:
                    case Opcode::CmpLe: case Opcode::CmpGe:
                    case Opcode::LAnd:  case Opcode::LOr:
                    case Opcode::Store: {
                        auto* bi = reinterpret_cast<const BinaryInstr*>(h);
                        countUse(bi->arg0); countUse(bi->arg1);
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
                        auto* ci = reinterpret_cast<const CondBranchInstr*>(h);
                        countUse(ci->cond);
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
                    default: break;
                }
            }
        }
    }

    // Remove instructions with zero uses (except side-effectful ones)
    for (auto& fn : functions) {
        for (auto& blk : fn.blocks) {
            blk.instructions.erase(
                std::remove_if(blk.instructions.begin(), blk.instructions.end(),
                    [&](IRValueRef ref) {
                        const InstrHeader* h = getInstr(ref);
                        // Keep: terminators, stores, calls (side effects)
                        switch (h->op) {
                            case Opcode::Branch:
                            case Opcode::CondBranch:
                            case Opcode::Return:
                            case Opcode::Store:
                            case Opcode::Call:
                                return false;  // never remove
                            default:
                                return useCounts.count(ref) == 0;
                        }
                    }),
                blk.instructions.end()
            );
        }
    }
}
