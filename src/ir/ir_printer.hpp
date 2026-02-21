#pragma once
// ir_printer.hpp - Pretty-print Umbra IR in LLVM-style text

#include "umbra_ir.hpp"
#include <iostream>
#include <sstream>

inline std::string fmtRef(const IRProgram& prog, IRValueRef ref) {
    if (ref == NullRef) return "void";
    if (prog.isParam(ref)) {
        uint32_t idx = IRProgram::paramIndex(ref);
        return std::string("%p") + std::to_string(idx);
    }
    return std::string("%") + std::to_string(ref);
}

inline void printInstruction(const IRProgram& prog, IRValueRef ref,
                              std::ostream& out) {
    const InstrHeader* h = prog.getInstr(ref);
    std::string resultStr = fmtRef(prog, ref);
    IRType rt = h->resultType;

    auto binOp = [&](const char* name) {
        const BinaryInstr* bi = reinterpret_cast<const BinaryInstr*>(h);
        out << "  " << resultStr << " = " << name << " "
            << irTypeName(rt) << " "
            << fmtRef(prog, bi->arg0) << ", "
            << fmtRef(prog, bi->arg1) << "\n";
    };
    auto unOp = [&](const char* name) {
        const UnaryInstr* ui = reinterpret_cast<const UnaryInstr*>(h);
        out << "  " << resultStr << " = " << name << " "
            << irTypeName(rt) << " "
            << fmtRef(prog, ui->arg0) << "\n";
    };

    switch (h->op) {
        case Opcode::ConstInt: {
            const ConstIntInstr* ci = reinterpret_cast<const ConstIntInstr*>(h);
            out << "  " << resultStr << " = const " << irTypeName(rt)
                << " " << ci->value << "\n";
            break;
        }
        case Opcode::ConstBool: {
            const ConstBoolInstr* cb = reinterpret_cast<const ConstBoolInstr*>(h);
            out << "  " << resultStr << " = const i1 "
                << (cb->value ? "true" : "false") << "\n";
            break;
        }
        case Opcode::Add:   binOp("add");    break;
        case Opcode::Sub:   binOp("sub");    break;
        case Opcode::Mul:   binOp("mul");    break;
        case Opcode::Div:   binOp("div");    break;
        case Opcode::Rem:   binOp("rem");    break;
        case Opcode::And:   binOp("and");    break;
        case Opcode::Or:    binOp("or");     break;
        case Opcode::Xor:   binOp("xor");    break;
        case Opcode::Shl:   binOp("shl");    break;
        case Opcode::Shr:   binOp("shr");    break;
        case Opcode::CRC32: binOp("crc32");  break;
        case Opcode::CmpEq: binOp("cmpeq"); break;
        case Opcode::CmpNe: binOp("cmpne"); break;
        case Opcode::CmpLt: binOp("cmplt"); break;
        case Opcode::CmpGt: binOp("cmpgt"); break;
        case Opcode::CmpLe: binOp("cmple"); break;
        case Opcode::CmpGe: binOp("cmpge"); break;
        case Opcode::LAnd:  binOp("land");  break;
        case Opcode::LOr:   binOp("lor");   break;
        case Opcode::Store: {
            const BinaryInstr* bi = reinterpret_cast<const BinaryInstr*>(h);
            out << "  store " << fmtRef(prog, bi->arg1)
                << " -> [" << fmtRef(prog, bi->arg0) << "]\n";
            break;
        }
        case Opcode::Load:   unOp("load");   break;
        case Opcode::ZExt:   unOp("zext");   break;
        case Opcode::SExt:   unOp("sext");   break;
        case Opcode::LNot:   unOp("lnot");   break;
        case Opcode::IsNull: unOp("isnull"); break;
        case Opcode::RotateRight: {
            const RotateRightInstr* ri = reinterpret_cast<const RotateRightInstr*>(h);
            out << "  " << resultStr << " = rotr " << irTypeName(rt)
                << " " << fmtRef(prog, ri->arg0)
                << ", " << ri->shift << "\n";
            break;
        }
        case Opcode::GEP: {
            const GEPInstr* gi = reinterpret_cast<const GEPInstr*>(h);
            out << "  " << resultStr << " = gep ptr "
                << fmtRef(prog, gi->base)
                << " + " << gi->byteOffset << "\n";
            break;
        }
        case Opcode::Branch: {
            const BranchInstr* bi = reinterpret_cast<const BranchInstr*>(h);
            out << "  br block" << bi->targetBlock << "\n";
            break;
        }
        case Opcode::CondBranch: {
            const CondBranchInstr* ci = reinterpret_cast<const CondBranchInstr*>(h);
            out << "  condbr " << fmtRef(prog, ci->cond)
                << ", block" << ci->trueBlock
                << ", block" << ci->falseBlock << "\n";
            break;
        }
        case Opcode::Return: {
            const ReturnInstr* ri = reinterpret_cast<const ReturnInstr*>(h);
            if (ri->value == NullRef) out << "  ret void\n";
            else out << "  ret " << fmtRef(prog, ri->value) << "\n";
            break;
        }
        case Opcode::Phi: {
            const PhiHeader* ph = reinterpret_cast<const PhiHeader*>(h);
            const PhiEntry* entries = reinterpret_cast<const PhiEntry*>(ph + 1);
            out << "  " << resultStr << " = phi " << irTypeName(rt) << " [";
            for (uint32_t i = 0; i < ph->numEntries; ++i) {
                if (i) out << ", ";
                out << fmtRef(prog, entries[i].value)
                    << ":block" << entries[i].fromBlock;
            }
            out << "]\n";
            break;
        }
        case Opcode::Call: {
            const CallHeader* ch = reinterpret_cast<const CallHeader*>(h);
            const IRValueRef* args = reinterpret_cast<const IRValueRef*>(ch + 1);
            out << "  " << resultStr << " = call " << irTypeName(rt)
                << " @0x" << std::hex << ch->funcPtr << std::dec << "(";
            for (uint32_t i = 0; i < ch->numArgs; ++i) {
                if (i) out << ", ";
                out << fmtRef(prog, args[i]);
            }
            out << ")\n";
            break;
        }
    }
}

inline void printIR(const IRProgram& prog, std::ostream& out) {
    for (const IRFunction& fn : prog.functions) {
        out << "define " << irTypeName(fn.returnType) << " " << fn.name << "(";
        for (size_t i = 0; i < fn.paramTypes.size(); ++i) {
            if (i) out << ", ";
            out << irTypeName(fn.paramTypes[i]) << " %p" << i
                << "[" << fn.paramNames[i] << "]";
        }
        out << ") {\n";

        for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
            const BasicBlock& blk = fn.blocks[bi];
            out << "block" << bi << " [" << blk.name << "]:\n";
            for (IRValueRef ref : blk.instructions) {
                printInstruction(prog, ref, out);
            }
        }
        out << "}\n\n";
    }
}
