// sql_runner.cpp - SQL pipeline: parse → plan → IR codegen → JIT → execute
//
// Execution path:
//   SQL string
//     → Lexer/Parser → AST
//     → QueryPlanner → OperatorTranslator tree (produce/consume → Umbra IR)
//     → FlyingStartBackend: Umbra IR → real native machine code via asmJIT
//     → JitRuntime::add() → callable function pointer
//     → fn() → result rows collected via PrintTranslator trampoline

#include "sql_runner.hpp"
#include "sql_parser.hpp"
#include "sql_catalog.hpp"
#include "sql_planner.hpp"

#include "../operators/compilation_context.hpp"
#include "../operators/operator_base.hpp"
#include "../ir/umbra_ir.hpp"
#include "../ir/ir_printer.hpp"
#include "../backend/flying_start.hpp"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <functional>

// ─── PrintTranslator ──────────────────────────────────────────────────────────
// Root consumer operator. Emits a Call IR instruction to a trampoline that
// appends each result row to a vector.
// This follows Layer 1 of the Tidy Tuples 5-layer architecture:
// the operator emits a Call into Layer 5 (codegen API) which generates Umbra IR.

struct PrintTranslator : OperatorTranslator {
    const std::vector<IU*>&           outputIUs;
    std::vector<std::vector<int64_t>> rows;

    PrintTranslator(CompilationContext& ctx, const std::vector<IU*>& ius)
        : OperatorTranslator(ctx), outputIUs(ius) {}

    void produce(OperatorTranslator* /*parent*/) override {
        // PrintTranslator is always the root — produce() is never called on it.
    }

    void consume(ConsumerScope& scope, OperatorTranslator* /*child*/) override {
        IRProgram& prog = ctx->program;

        int nCols = (int)outputIUs.size();
        if (nCols > 8)
            throw std::runtime_error("SELECT list too wide (max 8 columns)");

        // Embed 'this' as a constant pointer so the trampoline can append rows.
        IRValueRef selfPtr  = prog.addConstInt(
            reinterpret_cast<int64_t>(this), IRType::Ptr);
        IRValueRef nColsRef = prog.addConstInt(nCols, IRType::Int64);

        // Collect IRValueRefs for each output column.
        std::vector<IRValueRef> callArgs = {selfPtr, nColsRef};
        for (IU* iu : outputIUs)
            callArgs.push_back(scope.get(iu).valueRef);
        // Pad to exactly 8 value slots (trampoline has fixed arity).
        while ((int)callArgs.size() < 2 + 8)
            callArgs.push_back(prog.addConstInt(0, IRType::Int64));

        // Fixed-arity trampoline: appends a result row to this->rows.
        using TrampolineFn = void(*)(PrintTranslator*, int64_t,
                                     int64_t, int64_t, int64_t, int64_t,
                                     int64_t, int64_t, int64_t, int64_t);

        static TrampolineFn trampoline =
            [](PrintTranslator* self, int64_t nCols,
               int64_t v0, int64_t v1, int64_t v2, int64_t v3,
               int64_t v4, int64_t v5, int64_t v6, int64_t v7) {
                int64_t vals[] = {v0, v1, v2, v3, v4, v5, v6, v7};
                self->rows.emplace_back(vals, vals + nCols);
            };

        prog.addCall(IRType::Void,
                     reinterpret_cast<uint64_t>(trampoline),
                     callArgs);
    }
};

// ─── Operator tree printer ────────────────────────────────────────────────────
// Prints a human-readable operator tree (Layer 1 of Tidy Tuples).
// Each operator type is identified by dynamic_cast.

#include "../operators/scan_translator.hpp"
#include "../operators/select_translator.hpp"
#include "../operators/hash_join_translator.hpp"

static void deleteOperatorTree(OperatorTranslator* op,
                               std::unordered_set<OperatorTranslator*>& seen) {
    if (!op || seen.count(op)) return;
    seen.insert(op);

    if (auto* sel = dynamic_cast<SelectTranslator*>(op)) {
        deleteOperatorTree(sel->childOp(), seen);
    } else if (auto* hj = dynamic_cast<HashJoinTranslator*>(op)) {
        deleteOperatorTree(hj->buildSideOp(), seen);
        deleteOperatorTree(hj->probeSideOp(), seen);
    }

    delete op;
}

static void printOperatorTree(OperatorTranslator* op, std::ostream& out,
                               const std::string& prefix = "",
                               const std::string& childPrefix = "") {
    if (!op) return;

    if (auto* scan = dynamic_cast<ScanTranslator*>(op)) {
        out << prefix << "Scan(" << scan->tableName() << ")"
            << "  [" << scan->numOutputIUs() << " cols]\n";
    } else if (auto* sel = dynamic_cast<SelectTranslator*>(op)) {
        out << prefix << "Select(predicate)\n";
        printOperatorTree(sel->childOp(), out,
                          childPrefix + "  └─ ", childPrefix + "     ");
    } else if (auto* hj = dynamic_cast<HashJoinTranslator*>(op)) {
        out << prefix << "HashJoin  [" << hj->numBuildKeys() << " key(s)]\n";
        printOperatorTree(hj->buildSideOp(), out,
                          childPrefix + "  ├─ ", childPrefix + "  │  ");
        printOperatorTree(hj->probeSideOp(), out,
                          childPrefix + "  └─ ", childPrefix + "     ");
    } else {
        out << prefix << "Operator\n";
    }
}

// ─── runQuery ─────────────────────────────────────────────────────────────────

QueryResult runQuery(const std::string& sql,
                     const std::vector<const InMemoryTable*>& tables,
                     const ExplainOptions* explain,
                     std::ostream* out) {

    std::ostream& o = out ? *out : std::cout;

    // Step 1: Register tables in catalog
    SQLCatalog catalog;
    for (const InMemoryTable* tbl : tables)
        catalog.registerTable(tbl);

    // Step 2: Parse SQL string → SelectStmt AST
    SelectStmt stmt = parseSQL(sql);

    // Step 3: Create compilation context (owns the IRProgram)
    CompilationContext ctx;
    ctx.program.beginFunction("sql_query", IRType::Void, {});

    // Step 4: Plan → build OperatorTranslator tree (Tidy Tuples Layer 1)
    QueryPlanner planner(ctx, catalog);
    PlanResult plan = planner.plan(stmt);

    bool planCleaned = false;
    auto cleanupPlan = [&]() {
        if (planCleaned) return;
        std::unordered_set<OperatorTranslator*> seen;
        deleteOperatorTree(plan.root, seen);
        planCleaned = true;
    };
    struct CleanupGuard {
        std::function<void()> fn;
        ~CleanupGuard() { fn(); }
    } cleanupGuard{cleanupPlan};

    // ── EXPLAIN: operator tree ─────────────────────────────────────────────
    if (explain && explain->showTree) {
        o << "\n── Operator Tree (Tidy Tuples Layer 1: produce/consume) ────────\n";
        printOperatorTree(plan.root, o, "  ", "  ");
        o << "\n";
        o << "  Output columns: ";
        for (size_t i = 0; i < plan.columnNames.size(); ++i) {
            if (i) o << ", ";
            o << plan.columnNames[i];
        }
        o << "\n";
    }

    // Step 5: Create PrintTranslator as the root consumer.
    PrintTranslator printer(ctx, plan.outputIUs);

    // Step 6: Trigger produce/consume — generates all Umbra IR.
    plan.root->produce(&printer);
    ctx.program.addReturn();

    // Step 7: Run DCE
    size_t instrsBefore = 0;
    for (auto& fn : ctx.program.functions)
        for (auto& blk : fn.blocks)
            instrsBefore += blk.instructions.size();

    ctx.program.eliminateDeadCode();

    size_t instrsAfter = 0;
    for (auto& fn : ctx.program.functions)
        for (auto& blk : fn.blocks)
            instrsAfter += blk.instructions.size();

    // ── EXPLAIN: Tidy Tuples / Umbra IR ───────────────────────────────────
    if (explain && explain->showTT) {
        o << "\n── Umbra IR  (Tidy Tuples → IR, after DCE) ─────────────────────\n";
        o << "── instrData: " << ctx.program.instrData.size() << " bytes, "
          << instrsAfter << " instructions"
          << " (DCE removed " << (instrsBefore - instrsAfter) << ")\n\n";
        printIR(ctx.program, o);
    }

    // Step 8: Compile via Flying Start → real native machine code.
    FlyingStartBackend backend;
    FlyingStartBackend::QueryFn fn = backend.compile(
        ctx.program, ctx.program.functions[0]);

    // ── EXPLAIN: Flying Start assembly ────────────────────────────────────
    if (explain && explain->showFS) {
        o << "\n── Flying Start  (Umbra IR → native code via asmJIT) ───────────\n";
        backend.dumpAssembly(o);
    }

    // If EXPLAIN mode: don't execute — return empty result.
    if (explain) {
        backend.release(fn);
        QueryResult qr;
        qr.columnNames = plan.columnNames;
        cleanupPlan();
        return qr;
    }

    // Step 9: Execute the JIT-compiled function.
    backend.execute(fn);

    // Step 10: Release JIT memory
    backend.release(fn);

    // Step 11: Collect results
    QueryResult qr;
    qr.columnNames = plan.columnNames;
    qr.rows        = std::move(printer.rows);

    // Apply LIMIT
    if (stmt.limit >= 0 && (int64_t)qr.rows.size() > stmt.limit)
        qr.rows.resize((size_t)stmt.limit);

    cleanupPlan();
    return qr;
}
