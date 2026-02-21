#pragma once
// codegen_types.hpp - Layer 5 of Tidy Tuples: Codegen API
//
// C++ wrapper types that use operator overloading to emit Umbra IR
// instructions. Each type wraps an IRValueRef and a pointer to the
// IRProgram being built.
//
// Matches the paper's description:
//   "Codegen offers classes to generate code for primitive types and
//    uses C++ operator overloading to make it convenient to use."

#include "../ir/umbra_ir.hpp"

// Forward declarations
struct Bool;
struct Int32;
struct Int64;
struct UInt64;
struct Double;
template<typename T> struct Ptr;

// ─── CodegenContext ────────────────────────────────────────────────────────────
// Thread through everything: holds the IRProgram and current insertion point.
struct CodegenContext {
    IRProgram& prog;

    CodegenContext(IRProgram& p) : prog(p) {}

    // Delegation helpers so wrapper types don't need to go through prog directly
    IRValueRef emitBinary(Opcode op, IRType t, IRValueRef a, IRValueRef b) {
        return prog.addBinary(op, t, a, b);
    }
    IRValueRef emitUnary(Opcode op, IRType t, IRValueRef a) {
        return prog.addUnary(op, t, a);
    }
    IRValueRef emitConstInt(int64_t v, IRType t = IRType::Int64) {
        return prog.addConstInt(v, t);
    }
    IRValueRef emitConstBool(bool v) {
        return prog.addConstBool(v);
    }
    IRValueRef emitGEP(IRValueRef base, int64_t offset) {
        return prog.addGEP(base, offset);
    }
    IRValueRef emitLoad(IRType t, IRValueRef ptr) {
        return prog.addLoad(t, ptr);
    }
    IRValueRef emitStore(IRValueRef ptr, IRValueRef val) {
        return prog.addStore(ptr, val);
    }
    uint32_t addBlock(const std::string& name) { return prog.addBlock(name); }
    void setBlock(uint32_t blkIdx) {
        prog.setInsertionPoint(prog.currentFnIdx, blkIdx);
    }
    uint32_t currentBlock() const { return prog.currentBlkIdx; }
};

// ─── Bool ─────────────────────────────────────────────────────────────────────
struct Bool {
    IRValueRef ref;
    CodegenContext* ctx;

    Bool(IRValueRef r, CodegenContext* c) : ref(r), ctx(c) {}

    Bool operator&&(const Bool& o) const {
        return {ctx->emitBinary(Opcode::LAnd, IRType::Bool, ref, o.ref), ctx};
    }
    Bool operator||(const Bool& o) const {
        return {ctx->emitBinary(Opcode::LOr, IRType::Bool, ref, o.ref), ctx};
    }
    Bool lnot() const {
        return {ctx->emitUnary(Opcode::LNot, IRType::Bool, ref), ctx};
    }
    bool isConstTrue() const {
        return ctx->prog.isConstant(ref) &&
               ctx->prog.constIntValue(ref) != 0;
    }
};

inline Bool makeBool(CodegenContext& ctx, bool v) {
    return {ctx.emitConstBool(v), &ctx};
}

// ─── Int64 ────────────────────────────────────────────────────────────────────
struct Int64 {
    IRValueRef ref;
    CodegenContext* ctx;

    Int64(IRValueRef r, CodegenContext* c) : ref(r), ctx(c) {}

    Int64 operator+(const Int64& o) const {
        return {ctx->emitBinary(Opcode::Add, IRType::Int64, ref, o.ref), ctx};
    }
    Int64 operator-(const Int64& o) const {
        return {ctx->emitBinary(Opcode::Sub, IRType::Int64, ref, o.ref), ctx};
    }
    Int64 operator*(const Int64& o) const {
        return {ctx->emitBinary(Opcode::Mul, IRType::Int64, ref, o.ref), ctx};
    }
    Int64 operator/(const Int64& o) const {
        return {ctx->emitBinary(Opcode::Div, IRType::Int64, ref, o.ref), ctx};
    }
    Int64 operator%(const Int64& o) const {
        return {ctx->emitBinary(Opcode::Rem, IRType::Int64, ref, o.ref), ctx};
    }
    Int64 operator&(const Int64& o) const {
        return {ctx->emitBinary(Opcode::And, IRType::Int64, ref, o.ref), ctx};
    }
    Int64 operator|(const Int64& o) const {
        return {ctx->emitBinary(Opcode::Or, IRType::Int64, ref, o.ref), ctx};
    }
    Int64 operator^(const Int64& o) const {
        return {ctx->emitBinary(Opcode::Xor, IRType::Int64, ref, o.ref), ctx};
    }

    Bool operator==(const Int64& o) const {
        return {ctx->emitBinary(Opcode::CmpEq, IRType::Bool, ref, o.ref), ctx};
    }
    Bool operator!=(const Int64& o) const {
        return {ctx->emitBinary(Opcode::CmpNe, IRType::Bool, ref, o.ref), ctx};
    }
    Bool operator<(const Int64& o) const {
        return {ctx->emitBinary(Opcode::CmpLt, IRType::Bool, ref, o.ref), ctx};
    }
    Bool operator>(const Int64& o) const {
        return {ctx->emitBinary(Opcode::CmpGt, IRType::Bool, ref, o.ref), ctx};
    }
    Bool operator<=(const Int64& o) const {
        return {ctx->emitBinary(Opcode::CmpLe, IRType::Bool, ref, o.ref), ctx};
    }
    Bool operator>=(const Int64& o) const {
        return {ctx->emitBinary(Opcode::CmpGe, IRType::Bool, ref, o.ref), ctx};
    }
};

inline Int64 makeInt64(CodegenContext& ctx, int64_t v) {
    return {ctx.emitConstInt(v, IRType::Int64), &ctx};
}

// ─── UInt64 ───────────────────────────────────────────────────────────────────
// Matches the paper's hash computation example:
//   hash1 = hash1.crc32(v); hash2 = hash2.crc32(v);
//   UInt64 hash = hash1 ^ hash2.rotateRight(32);
//   hash *= 11400714819323198485;
struct UInt64 {
    IRValueRef ref;
    CodegenContext* ctx;

    UInt64(IRValueRef r, CodegenContext* c) : ref(r), ctx(c) {}

    UInt64 operator+(const UInt64& o) const {
        return {ctx->emitBinary(Opcode::Add, IRType::UInt64, ref, o.ref), ctx};
    }
    UInt64 operator*(const UInt64& o) const {
        return {ctx->emitBinary(Opcode::Mul, IRType::UInt64, ref, o.ref), ctx};
    }
    UInt64 operator^(const UInt64& o) const {
        return {ctx->emitBinary(Opcode::Xor, IRType::UInt64, ref, o.ref), ctx};
    }
    UInt64 operator&(const UInt64& o) const {
        return {ctx->emitBinary(Opcode::And, IRType::UInt64, ref, o.ref), ctx};
    }

    // CRC32: matches paper's hash generation
    UInt64 crc32(const UInt64& v) const {
        return {ctx->emitBinary(Opcode::CRC32, IRType::UInt64, ref, v.ref), ctx};
    }

    // Rotate right: matches paper's "hash2.rotateRight(32)"
    UInt64 rotateRight(uint32_t bits) const {
        return {ctx->prog.addRotateRight(ref, bits), ctx};
    }

    Bool operator==(const UInt64& o) const {
        return {ctx->emitBinary(Opcode::CmpEq, IRType::Bool, ref, o.ref), ctx};
    }
};

inline UInt64 makeUInt64(CodegenContext& ctx, uint64_t v) {
    return {ctx.emitConstInt(static_cast<int64_t>(v), IRType::UInt64), &ctx};
}

// ─── Ptr<T> ───────────────────────────────────────────────────────────────────
// Typed pointer supporting load, store, and offset (GEP).
// The type parameter T determines what you load/store.
template<typename T>
struct Ptr {
    IRValueRef ref;
    CodegenContext* ctx;
    IRType elementType;  // IR type of the pointed-to value

    Ptr(IRValueRef r, CodegenContext* c, IRType et)
        : ref(r), ctx(c), elementType(et) {}

    T load() const {
        IRValueRef loaded = ctx->emitLoad(elementType, ref);
        return T{loaded, ctx};
    }

    void store(const T& val) const {
        ctx->emitStore(ref, val.ref);
    }

    // GEP: add a constant byte offset (lazy address calculation)
    Ptr<T> operator+(int64_t byteOffset) const {
        return {ctx->emitGEP(ref, byteOffset), ctx, elementType};
    }

    // Cast to untyped Ptr<void> equivalent for passing to calls
    IRValueRef rawRef() const { return ref; }
};

// Specialization for untyped byte pointer
struct BytePtr {
    IRValueRef ref;
    CodegenContext* ctx;

    BytePtr(IRValueRef r, CodegenContext* c) : ref(r), ctx(c) {}

    Int64 loadInt64(int64_t byteOffset = 0) const {
        IRValueRef ptr = byteOffset ? ctx->emitGEP(ref, byteOffset) : ref;
        return {ctx->emitLoad(IRType::Int64, ptr), ctx};
    }

    UInt64 loadUInt64(int64_t byteOffset = 0) const {
        IRValueRef ptr = byteOffset ? ctx->emitGEP(ref, byteOffset) : ref;
        return {ctx->emitLoad(IRType::UInt64, ptr), ctx};
    }

    void storeInt64(const Int64& val, int64_t byteOffset = 0) const {
        IRValueRef ptr = byteOffset ? ctx->emitGEP(ref, byteOffset) : ref;
        ctx->emitStore(ptr, val.ref);
    }

    void storeUInt64(const UInt64& val, int64_t byteOffset = 0) const {
        IRValueRef ptr = byteOffset ? ctx->emitGEP(ref, byteOffset) : ref;
        ctx->emitStore(ptr, val.ref);
    }

    BytePtr offset(int64_t byteOffset) const {
        return {ctx->emitGEP(ref, byteOffset), ctx};
    }
};

// ─── Control Flow constructs ───────────────────────────────────────────────────
// These match the paper's If and Loop classes.

// If construct: emits condbr → [trueBlock, falseBlock] → mergeBlock
// Usage:
//   {
//     If ifStmt(ctx, condition);
//     // code in true branch
//   }  // destructor emits branch to merge, sets insertion to merge
struct IfStmt {
    CodegenContext* ctx;
    uint32_t trueBlock;
    uint32_t falseBlock;
    uint32_t mergeBlock;

    IfStmt(CodegenContext& c, Bool condition)
        : ctx(&c)
    {
        trueBlock  = c.addBlock("if_true");
        falseBlock = c.addBlock("if_false");
        mergeBlock = c.addBlock("if_merge");

        // Emit conditional branch in current block
        c.prog.addCondBranch(condition.ref, trueBlock, falseBlock);

        // Switch to true block
        c.setBlock(trueBlock);
    }

    // Call to switch to else branch
    void els() {
        ctx->prog.addBranch(mergeBlock);
        ctx->setBlock(falseBlock);
    }

    // Destructor: emit jump to merge, switch insertion there
    ~IfStmt() {
        // If we're not already terminated
        auto& blk = ctx->prog.currentBlock();
        if (blk.instructions.empty() ||
            (ctx->prog.getInstr(blk.instructions.back())->op != Opcode::Branch &&
             ctx->prog.getInstr(blk.instructions.back())->op != Opcode::CondBranch &&
             ctx->prog.getInstr(blk.instructions.back())->op != Opcode::Return)) {
            ctx->prog.addBranch(mergeBlock);
        }
        ctx->setBlock(mergeBlock);
    }
};

// Loop construct with PHI nodes for loop variables (SSA form)
// Usage:
//   Loop loop(ctx, "loop_name", entryCondition, {initialVal});
//   // use loop.getVar<0>() inside body
//   ...
//   loop.done(continueCondition, {nextVal});
struct LoopStmt {
    CodegenContext* ctx;
    uint32_t headerBlock;
    uint32_t bodyBlock;
    uint32_t exitBlock;
    std::vector<IRValueRef> phiRefs;  // PHI node refs (loop variables)
    std::vector<IRValueRef> initVals;
    uint32_t entryBlock;  // block before the loop
    IRType varType;

    // Creates: entryBlock --(cond)--> bodyBlock | exitBlock
    //          bodyBlock --> (back edge to header)
    LoopStmt(CodegenContext& c,
              const std::string& name,
              Bool entryCondition,
              std::vector<IRValueRef> initialVals,
              IRType varTy = IRType::Int64)
        : ctx(&c), initVals(initialVals), varType(varTy)
    {
        entryBlock  = c.currentBlock();
        headerBlock = c.addBlock(name + "_header");
        bodyBlock   = c.addBlock(name + "_body");
        exitBlock   = c.addBlock(name + "_exit");

        // Jump to header unconditionally
        c.prog.addBranch(headerBlock);
        c.setBlock(headerBlock);

        // Create PHI nodes for each loop variable
        // Initially only have incoming from entry block
        for (IRValueRef initVal : initialVals) {
            IRType t = (varTy != IRType::Void)
                       ? varTy
                       : c.prog.typeOf(initVal);
            // Placeholder PHI — we'll add the back-edge entry in done()
            IRValueRef phi = c.prog.addPhi(t, {{initVal, entryBlock}});
            phiRefs.push_back(phi);
        }

        // Branch to body or exit based on condition
        c.prog.addCondBranch(entryCondition.ref, bodyBlock, exitBlock);
        c.setBlock(bodyBlock);
    }

    IRValueRef getVar(size_t idx) const { return phiRefs[idx]; }

    // Call at end of loop body with new values and continue condition
    void done(Bool continueCondition, std::vector<IRValueRef> nextVals) {
        uint32_t backEdgeBlock = ctx->currentBlock();

        // Branch back to header or exit
        ctx->prog.addCondBranch(continueCondition.ref, headerBlock, exitBlock);

        // Patch PHI nodes with back-edge values
        // We need to update the PHI nodes in the header block
        // The PHI nodes are in headerBlock's instruction list
        for (size_t i = 0; i < phiRefs.size() && i < nextVals.size(); ++i) {
            IRValueRef phiRef = phiRefs[i];
            PhiHeader* ph = reinterpret_cast<PhiHeader*>(
                ctx->prog.getInstrMut(phiRef));
            // Resize instrData to add one more PhiEntry
            // The PHI is always at the end of what was emitted when created,
            // so we can append safely only if it's the last thing written—
            // for simplicity in this POC we store PHI entries in a side map
            // and skip patching the raw bytes; instead store in phiExtraEntries
            (void)ph;
            // Store back-edge info for printing
            phiBackEdge[phiRef] = {nextVals[i], backEdgeBlock};
        }

        ctx->setBlock(exitBlock);
    }

    // Extra back-edge entries (for printing / Flying Start use)
    struct BackEdge { IRValueRef nextVal; uint32_t fromBlock; };
    std::unordered_map<IRValueRef, BackEdge> phiBackEdge;

    uint32_t breakBlock() const { return exitBlock; }
    uint32_t continueBlock() const { return bodyBlock; }
};
