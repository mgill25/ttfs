// scan_translator.cpp
#include "scan_translator.hpp"
#include <stdexcept>

ScanTranslator::ScanTranslator(CompilationContext& ctx,
                                const InMemoryTable* tbl)
    : OperatorTranslator(ctx), table(tbl) {
    // Create one IU per column
    for (size_t i = 0; i < tbl->columnNames.size(); ++i) {
        IU* iu = ctx.createIU(tbl->name + "." + tbl->columnNames[i],
                               tbl->columnTypes[i]);
        colIUs.push_back(iu);
        outputIUs.push_back(iu);
    }
}

IU* ScanTranslator::getIU(const std::string& colName) const {
    for (IU* iu : colIUs) {
        // IU name is "table.col"
        std::string suffix = "." + colName;
        if (iu->name.size() >= suffix.size() &&
            iu->name.substr(iu->name.size() - suffix.size()) == suffix)
            return iu;
    }
    throw std::runtime_error("Column not found: " + colName);
}

void ScanTranslator::produce(OperatorTranslator* parent) {
    CodegenContext& cg = ctx->codegen;
    IRProgram& prog    = ctx->program;

    // For each column, embed the column data pointer as a constant address.
    // In a real system this would be a function parameter (pointer to storage).
    // For the POC, we use the actual runtime address of the column array via Call.
    //
    // We generate:
    //   i = 0
    //   while (i < numRows):
    //     for each column:
    //       val = column_data[i]  (emitted as a "call to load" for simplicity)
    //     parent->consume(scope)
    //     i = i + 1

    // Constants
    IRValueRef zero    = prog.addConstInt(0, IRType::Int64);
    IRValueRef numRows = prog.addConstInt((int64_t)table->numRows, IRType::Int64);
    IRValueRef one     = prog.addConstInt(1, IRType::Int64);

    // Loop header block
    uint32_t headerBlk = prog.addBlock(table->name + "_scan_header");
    uint32_t bodyBlk   = prog.addBlock(table->name + "_scan_body");
    uint32_t exitBlk   = prog.addBlock(table->name + "_scan_exit");

    // Record which block we're branching from (entry block, before we switch to headerBlk)
    uint32_t entryBlk = prog.currentBlkIdx;
    prog.addBranch(headerBlk);
    prog.setInsertionPoint(prog.currentFnIdx, headerBlk);

    // PHI for loop index i.
    // Pre-allocate 2 entries: [0] = initial value from entry, [1] = back-edge (patched later).
    // Use NullRef as a placeholder for the back-edge entry that we patch after generating iNext.
    IRValueRef iPhi = prog.addPhi(IRType::Int64, {{zero, entryBlk}, {NullRef, 0}});

    // Check i < numRows
    IRValueRef cond = prog.addBinary(Opcode::CmpLt, IRType::Bool, iPhi, numRows);
    prog.addCondBranch(cond, bodyBlk, exitBlk);

    prog.setInsertionPoint(prog.currentFnIdx, bodyBlk);

    // For each column, emit a call to load column[i]
    // We pass the column data pointer + index to a helper function
    ConsumerScope scope;

    for (size_t col = 0; col < colIUs.size(); ++col) {
        // Load column[i]: call our runtime helper
        // Helper signature: int64_t loadColumnValue(int64_t* colPtr, int64_t idx)
        const std::vector<int64_t>& colData = table->columns[col];
        uint64_t colPtr = reinterpret_cast<uint64_t>(colData.data());

        // Emit: colPtrConst = const ptr <address>
        IRValueRef colPtrRef = prog.addConstInt((int64_t)colPtr, IRType::Ptr);
        // Emit: GEP to element i (stride = 8 bytes per int64_t)
        // We can't do dynamic GEP with variable index in this simple IR,
        // so we emit a Call to a helper that does: data[i]
        static auto loadFn = [](const int64_t* ptr, int64_t idx) -> int64_t {
            return ptr[idx];
        };
        // Cast to function pointer
        using LoadFnType = int64_t(*)(const int64_t*, int64_t);
        uint64_t fnAddr = reinterpret_cast<uint64_t>((LoadFnType)loadFn);

        IRValueRef val = prog.addCall(IRType::Int64, fnAddr,
                                       {colPtrRef, iPhi});

        SQLType stype = table->columnTypes[col];
        scope.bind(colIUs[col],
                   makeNonNullSQLValue(val, stype, cg));
    }

    if (parent) {
        parent->consume(scope, this);
    }
    // else: root scan with no consumer — just the loop (unusual)

    // Increment i: iNext = i + 1  (this is the back-edge value for the PHI)
    IRValueRef iNext = prog.addBinary(Opcode::Add, IRType::Int64, iPhi, one);

    // Record which block the back-edge comes from (bodyBlk = current block)
    uint32_t backEdgeBlk = prog.currentBlkIdx;

    // Patch PHI entry [1] with the real back-edge value now that iNext is known.
    prog.patchPhiEntry(iPhi, 1, iNext, backEdgeBlk);

    // Back-edge: jump to header
    prog.addBranch(headerBlk);

    prog.setInsertionPoint(prog.currentFnIdx, exitBlk);
}
