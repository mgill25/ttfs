#pragma once
// sql_planner.hpp - AST → operator tree
//
// QueryPlanner translates a SelectStmt into an OperatorTranslator tree
// by going through 4 phases:
//   Phase 1: Create ScanTranslator per FROM table
//   Phase 2: Classify WHERE conjuncts (filter, join-key, post-join filter)
//   Phase 3: Build left-deep join tree (with filters pushed down)
//   Phase 4: Resolve SELECT list → output IUs

#include "sql_ast.hpp"
#include "sql_catalog.hpp"
#include "../operators/compilation_context.hpp"
#include "../operators/scan_translator.hpp"
#include "../operators/select_translator.hpp"
#include "../operators/hash_join_translator.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

// ─── Result of planning ───────────────────────────────────────────────────────
struct PlanResult {
    OperatorTranslator* root;    // the top-most operator (consumer feeds here)
    std::vector<IU*>    outputIUs;  // IUs in SELECT list order
    std::vector<std::string> columnNames;  // display names for output cols
};

class QueryPlanner {
public:
    QueryPlanner(CompilationContext& ctx, const SQLCatalog& catalog)
        : ctx(ctx), catalog(catalog) {}

    // Main entry point: plan a parsed SELECT statement.
    // Returns the root operator and the ordered output IUs.
    PlanResult plan(const SelectStmt& stmt);

private:
    CompilationContext& ctx;
    const SQLCatalog&   catalog;

    // alias → ScanTranslator
    std::unordered_map<std::string, ScanTranslator*> scans;
    // alias → table name (for * expansion)
    std::unordered_map<std::string, const InMemoryTable*> aliasTables;

    // Resolve a ColumnRef to an IU.
    // If tableAlias is empty, tries all scans.
    IU* resolveColumn(const ColumnRef& ref) const;

    // Find which alias owns a ColumnRef.
    std::string resolveAlias(const ColumnRef& ref) const;

    // Collect top-level AND conjuncts from an expression.
    static void flattenAnd(const Expr* e, std::vector<const Expr*>& out);

    // Classify one conjunct.
    // Returns the set of aliases it references.
    static std::vector<std::string> conjunctAliases(const Expr* e);

    // Collect aliases referenced in an expression.
    static void collectAliases(const Expr* e, std::vector<std::string>& out);

    // Emit IR code for an expression given the current ConsumerScope.
    SQLValue evalExpr(const Expr* e, ConsumerScope& scope);
};
