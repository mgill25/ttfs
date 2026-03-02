#pragma once
// umbra_ir.hpp - Umbra IR: custom intermediate representation
//
// Design matches the paper:
// - Instructions stored in a flat byte array (instrData)
// - Values referenced by 4-byte offsets (IRValueRef) into instrData
// - Variable-length instructions (each opcode determines length)
// - Constant folding at insertion time
// - Dead code elimination in a single pass

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <cassert>
#include <optional>
#include <functional>

// ─── Value references ─────────────────────────────────────────────────────────
// A 4-byte offset into the flat instruction array.
// Using offsets (not pointers) saves space and stays valid after realloc.
using IRValueRef = uint32_t;
constexpr IRValueRef NullRef = UINT32_MAX;

// ─── Types ────────────────────────────────────────────────────────────────────
enum class IRType : uint8_t {
    Void,
    Bool,
    Int32,
    Int64,
    UInt64,
    Double,
    Ptr,    // untyped pointer (i8* in LLVM terms)
};

inline const char* irTypeName(IRType t) {
    switch (t) {
        case IRType::Void:   return "void";
        case IRType::Bool:   return "i1";
        case IRType::Int32:  return "i32";
        case IRType::Int64:  return "i64";
        case IRType::UInt64: return "u64";
        case IRType::Double: return "f64";
        case IRType::Ptr:    return "ptr";
    }
    return "?";
}

// ─── Opcodes ──────────────────────────────────────────────────────────────────
enum class Opcode : uint8_t {
    // Constants
    ConstInt,     // ConstIntInstr
    ConstBool,    // ConstBoolInstr

    // Arithmetic (binary)
    Add,
    Sub,
    Mul,
    Div,
    Rem,

    // Bitwise / special (binary)
    And,
    Or,
    Xor,
    Shl,
    Shr,
    CRC32,        // x = crc32(seed, value)   — DB-specific

    // Unary
    RotateRight,  // (value, const_shift) — stored as unary + const operand
    ZExt,         // zero-extend to wider type
    SExt,         // sign-extend

    // Comparisons (binary → Bool)
    CmpEq,
    CmpNe,
    CmpLt,
    CmpGt,
    CmpLe,
    CmpGe,

    // Bool ops
    LAnd,
    LOr,
    LNot,         // unary

    // Memory
    Load,         // (ptr) → value
    Store,        // (ptr, value) → void

    // Address arithmetic (lazy address calc)
    GEP,          // GetElementPtr: (base_ptr, byte_offset_const) → ptr

    // Control flow
    Branch,       // unconditional: target block id
    CondBranch,   // cond, trueBlock, falseBlock
    Return,       // (value) or () for void

    // PHI node (SSA)
    Phi,

    // Function call (proxy / runtime helper)
    Call,

    // DB-specific
    IsNull,       // unary Bool → Bool (identity for nullable propagation)
};

// ─── Instruction structs ──────────────────────────────────────────────────────
// All stored packed in the byte array.  Each starts with opcode + resultType.

struct InstrHeader {
    Opcode op;
    IRType resultType;
};

struct ConstIntInstr {
    Opcode op; IRType resultType;
    int64_t value;
};

struct ConstBoolInstr {
    Opcode op; IRType resultType;
    bool value;
};

struct BinaryInstr {
    Opcode op; IRType resultType;
    IRValueRef arg0;
    IRValueRef arg1;
};

struct UnaryInstr {
    Opcode op; IRType resultType;
    IRValueRef arg0;
};

// RotateRight: value + compile-time shift amount
struct RotateRightInstr {
    Opcode op; IRType resultType;
    IRValueRef arg0;
    uint32_t shift;
};

// GEP: base pointer + constant byte offset
struct GEPInstr {
    Opcode op; IRType resultType;
    IRValueRef base;
    int64_t byteOffset;
};

struct BranchInstr {
    Opcode op; IRType resultType;
    uint32_t targetBlock;
};

struct CondBranchInstr {
    Opcode op; IRType resultType;
    IRValueRef cond;
    uint32_t trueBlock;
    uint32_t falseBlock;
};

struct ReturnInstr {
    Opcode op; IRType resultType;
    IRValueRef value;   // NullRef for void return
};

// PHI node: N predecessors
// Stored as PhiHeader + N * PhiEntry
struct PhiHeader {
    Opcode op; IRType resultType;
    uint32_t numEntries;
};
struct PhiEntry {
    IRValueRef value;
    uint32_t fromBlock;
};

// Call instruction: runtime function call
// Stored as CallHeader + N * IRValueRef args
struct CallHeader {
    Opcode op; IRType resultType;
    uint64_t funcPtr;   // address of the C++ function (cast to uint64_t)
    uint32_t numArgs;
};

// ─── Basic block ──────────────────────────────────────────────────────────────
struct BasicBlock {
    std::string name;
    std::vector<IRValueRef> instructions;  // offsets into instrData
};

// ─── Function ─────────────────────────────────────────────────────────────────
struct IRFunction {
    std::string name;
    IRType returnType;
    std::vector<std::string> paramNames;
    std::vector<IRType> paramTypes;
    std::vector<BasicBlock> blocks;  // block 0 = entry

    // Params are also addressable as IRValueRef.
    // By convention, param i has ref = i (they live before instrData offset 0).
    // We use a separate paramRefs vector for clarity.
    std::vector<IRValueRef> paramRefs;
};

// ─── IRProgram ────────────────────────────────────────────────────────────────
// Central class: holds all instructions in instrData, all functions.
class IRProgram {
public:
    // Flat byte array — all instructions live here
    std::vector<uint8_t> instrData;

    // All functions in the program
    std::vector<IRFunction> functions;

    // Currently active insertion point
    uint32_t currentFnIdx  = 0;
    uint32_t currentBlkIdx = 0;

    // ── Creating functions and blocks ──────────────────────────────────────
    uint32_t beginFunction(const std::string& name,
                           IRType returnType,
                           std::vector<std::pair<std::string,IRType>> params);

    uint32_t addBlock(const std::string& name);
    void setInsertionPoint(uint32_t fnIdx, uint32_t blkIdx);

    IRFunction& currentFunction() { return functions[currentFnIdx]; }
    BasicBlock& currentBlock()    { return functions[currentFnIdx].blocks[currentBlkIdx]; }

    // ── Instruction builders ───────────────────────────────────────────────
    IRValueRef addConstInt(int64_t value, IRType t = IRType::Int64);
    IRValueRef addConstBool(bool value);

    IRValueRef addBinary(Opcode op, IRType resultType,
                         IRValueRef a, IRValueRef b);
    IRValueRef addUnary(Opcode op, IRType resultType, IRValueRef a);
    IRValueRef addRotateRight(IRValueRef val, uint32_t shift);
    IRValueRef addGEP(IRValueRef base, int64_t byteOffset);
    IRValueRef addLoad(IRType resultType, IRValueRef ptr);
    IRValueRef addStore(IRValueRef ptr, IRValueRef value);
    IRValueRef addBranch(uint32_t targetBlock);
    IRValueRef addCondBranch(IRValueRef cond,
                              uint32_t trueBlock, uint32_t falseBlock);
    IRValueRef addReturn(IRValueRef value = NullRef);
    IRValueRef addPhi(IRType resultType,
                      std::vector<std::pair<IRValueRef,uint32_t>> incoming);
    IRValueRef addCall(IRType resultType,
                       uint64_t funcPtr,
                       std::vector<IRValueRef> args);

    // Patch a PHI entry in-place (entry must already be allocated with a slot).
    // Use only when the PHI was created with a placeholder entry for the back-edge.
    void patchPhiEntry(IRValueRef phiRef, uint32_t entryIdx,
                       IRValueRef newValue, uint32_t fromBlock);

    // ── Instruction access ─────────────────────────────────────────────────
    // Return pointer to instruction at given offset (valid until instrData resizes)
    const InstrHeader* getInstr(IRValueRef ref) const {
        assert(ref < instrData.size());
        return reinterpret_cast<const InstrHeader*>(instrData.data() + ref);
    }
    InstrHeader* getInstrMut(IRValueRef ref) {
        assert(ref < instrData.size());
        return reinterpret_cast<InstrHeader*>(instrData.data() + ref);
    }

    // Return byte size of instruction at ref
    size_t instrSize(IRValueRef ref) const;

    // Is this ref a function parameter?
    bool isParam(IRValueRef ref) const {
        return (ref & 0xFF000000u) == 0xFF000000u;
    }
    // Encode/decode parameter refs (outside instrData space)
    static IRValueRef paramRef(uint32_t fnIdx, uint32_t paramIdx) {
        return 0xFF000000u | ((fnIdx & 0xFFu) << 16) | (paramIdx & 0xFFFFu);
    }
    static uint32_t paramIndex(IRValueRef ref) { return ref & 0xFFFFu; }

    // ── Optimizations ──────────────────────────────────────────────────────
    // Constant folding: done inside addBinary/addUnary
    // Returns NullRef if no fold possible
    IRValueRef tryConstFold(Opcode op, IRType t,
                            IRValueRef a, IRValueRef b);

    // Dead code elimination: remove instructions with zero uses
    // (terminators and stores are always kept)
    void eliminateDeadCode();

    // ── Helpers ────────────────────────────────────────────────────────────
    IRType typeOf(IRValueRef ref) const;
    bool isConstant(IRValueRef ref) const;
    int64_t constIntValue(IRValueRef ref) const;

private:
    // Write a struct T into instrData, return its offset
    template<typename T>
    IRValueRef emit(const T& instr) {
        IRValueRef offset = static_cast<IRValueRef>(instrData.size());
        instrData.resize(instrData.size() + sizeof(T));
        std::memcpy(instrData.data() + offset, &instr, sizeof(T));
        // Record that this instruction was added to the current block
        currentBlock().instructions.push_back(offset);
        return offset;
    }

    // Emit without adding to any block (for internal use)
    template<typename T>
    IRValueRef emitRaw(const T& instr) {
        IRValueRef offset = static_cast<IRValueRef>(instrData.size());
        instrData.resize(instrData.size() + sizeof(T));
        std::memcpy(instrData.data() + offset, &instr, sizeof(T));
        return offset;
    }

    // Constant deduplication: key = (opcode<<32 | value lower bits)
    std::unordered_map<uint64_t, IRValueRef> constIntCache;
};
