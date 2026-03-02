// sql_planner.cpp - AST → operator tree implementation

#include "sql_planner.hpp"
#include <stdexcept>
#include <algorithm>

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Collect top-level AND conjuncts.
void QueryPlanner::flattenAnd(const Expr* e, std::vector<const Expr*>& out) {
    if (e->kind == ExprKind::BinOp && e->binOp->op == BinOpKind::And) {
        flattenAnd(e->binOp->left.get(),  out);
        flattenAnd(e->binOp->right.get(), out);
    } else {
        out.push_back(e);
    }
}

// Collect all alias names referenced in an expression.
void QueryPlanner::collectAliases(const Expr* e, std::vector<std::string>& out) {
    if (e->kind == ExprKind::ColumnRef) {
        if (!e->colRef.tableAlias.empty()) {
            // Only add if not already present
            for (auto& a : out) if (a == e->colRef.tableAlias) return;
            out.push_back(e->colRef.tableAlias);
        }
    } else if (e->kind == ExprKind::BinOp) {
        collectAliases(e->binOp->left.get(),  out);
        collectAliases(e->binOp->right.get(), out);
    }
    // IntLiteral: no aliases
}

std::vector<std::string> QueryPlanner::conjunctAliases(const Expr* e) {
    std::vector<std::string> aliases;
    collectAliases(e, aliases);
    return aliases;
}

// ─── Column resolution ────────────────────────────────────────────────────────

std::string QueryPlanner::resolveAlias(const ColumnRef& ref) const {
    if (!ref.tableAlias.empty()) {
        if (scans.count(ref.tableAlias) == 0)
            throw std::runtime_error("Unknown table alias: " + ref.tableAlias);
        return ref.tableAlias;
    }
    // Unqualified: search all scans
    std::string found;
    for (auto& [alias, scan] : scans) {
        try {
            scan->getIU(ref.colName);
            if (!found.empty())
                throw std::runtime_error(
                    "Ambiguous column reference: " + ref.colName);
            found = alias;
        } catch (const std::runtime_error& e) {
            // Not in this scan — continue
            std::string msg = e.what();
            if (msg.find("Column not found") == std::string::npos)
                throw;
        }
    }
    if (found.empty())
        throw std::runtime_error("Column not found: " + ref.colName);
    return found;
}

IU* QueryPlanner::resolveColumn(const ColumnRef& ref) const {
    std::string alias = ref.tableAlias;
    if (alias.empty()) {
        alias = const_cast<QueryPlanner*>(this)->resolveAlias(ref);
    }
    auto it = scans.find(alias);
    if (it == scans.end())
        throw std::runtime_error("Unknown table alias: " + alias);
    return it->second->getIU(ref.colName);
}

// ─── evalExpr ─────────────────────────────────────────────────────────────────
// Emit IR for an expression given the current ConsumerScope.

SQLValue QueryPlanner::evalExpr(const Expr* e, ConsumerScope& scope) {
    switch (e->kind) {
    case ExprKind::ColumnRef:
        return scope.get(resolveColumn(e->colRef));

    case ExprKind::IntLiteral:
        return makeIntSQLValue(ctx.codegen, e->intVal);

    case ExprKind::BinOp: {
        const BinOpExpr* bop = e->binOp.get();
        SQLValue l = evalExpr(bop->left.get(),  scope);
        SQLValue r = evalExpr(bop->right.get(), scope);
        switch (bop->op) {
        case BinOpKind::Eq:  return l.eq(r);
        case BinOpKind::Ne:  return l.ne(r);
        case BinOpKind::Lt:  return l.lt(r);
        case BinOpKind::Gt:  return l.gt(r);
        case BinOpKind::Le:  return l.le(r);
        case BinOpKind::Ge:  return l.ge(r);
        case BinOpKind::Add: return l.add(r);
        case BinOpKind::Sub: return l.sub(r);
        case BinOpKind::And: {
            Bool b = l.asBool() && r.asBool();
            return {b.ref, NullRef, SQLType::Bool, &ctx.codegen};
        }
        case BinOpKind::Or: {
            Bool b = l.asBool() || r.asBool();
            return {b.ref, NullRef, SQLType::Bool, &ctx.codegen};
        }
        }
        break;
    }
    }
    throw std::runtime_error("evalExpr: unhandled expression kind");
}

// ─── plan() ──────────────────────────────────────────────────────────────────

PlanResult QueryPlanner::plan(const SelectStmt& stmt) {
    // ── Phase 1: Create ScanTranslators ──────────────────────────────────────
    std::vector<std::string> aliases;  // in FROM order
    for (const TableRef& ref : stmt.fromList) {
        const InMemoryTable* tbl = catalog.lookup(ref.tableName);
        auto* scan = new ScanTranslator(ctx, tbl);
        scans[ref.alias] = scan;
        aliasTables[ref.alias] = tbl;
        aliases.push_back(ref.alias);
    }

    // ── Phase 2: Classify WHERE conjuncts ────────────────────────────────────
    // conjuncts referencing 1 alias → per-alias filter list
    // conjuncts referencing 2 aliases with root Eq(col, col) → join keys
    // anything else → post-join filter

    struct JoinKey {
        std::string buildAlias;
        std::string probeAlias;
        IU*         buildIU;
        IU*         probeIU;
    };

    // per-alias filter conjuncts
    std::unordered_map<std::string, std::vector<const Expr*>> filterConjuncts;
    std::vector<JoinKey>      joinKeys;
    std::vector<const Expr*>  postJoinFilters;

    if (stmt.whereExpr) {
        std::vector<const Expr*> conjuncts;
        flattenAnd(stmt.whereExpr.get(), conjuncts);

        for (const Expr* c : conjuncts) {
            auto aliasSet = conjunctAliases(c);

            if (aliasSet.size() == 1) {
                filterConjuncts[aliasSet[0]].push_back(c);
            } else if (aliasSet.size() == 2 &&
                       c->kind == ExprKind::BinOp &&
                       c->binOp->op == BinOpKind::Eq &&
                       c->binOp->left->kind  == ExprKind::ColumnRef &&
                       c->binOp->right->kind == ExprKind::ColumnRef) {
                // Join key: Eq(col1, col2) referencing 2 different tables
                const ColumnRef& lcr = c->binOp->left->colRef;
                const ColumnRef& rcr = c->binOp->right->colRef;

                std::string la = lcr.tableAlias;
                std::string ra = rcr.tableAlias;
                if (la.empty()) la = const_cast<QueryPlanner*>(this)->resolveAlias(lcr);
                if (ra.empty()) ra = const_cast<QueryPlanner*>(this)->resolveAlias(rcr);

                // Determine which side is build vs probe based on FROM order
                auto lpos = std::find(aliases.begin(), aliases.end(), la) - aliases.begin();
                auto rpos = std::find(aliases.begin(), aliases.end(), ra) - aliases.begin();

                JoinKey jk;
                if (lpos <= rpos) {
                    jk.buildAlias = la;
                    jk.probeAlias = ra;
                    jk.buildIU    = scans[la]->getIU(lcr.colName);
                    jk.probeIU    = scans[ra]->getIU(rcr.colName);
                } else {
                    jk.buildAlias = ra;
                    jk.probeAlias = la;
                    jk.buildIU    = scans[ra]->getIU(rcr.colName);
                    jk.probeIU    = scans[la]->getIU(lcr.colName);
                }
                joinKeys.push_back(jk);
            } else if (aliasSet.size() == 0) {
                // Constant predicate — treat as post-join filter
                postJoinFilters.push_back(c);
            } else {
                postJoinFilters.push_back(c);
            }
        }
    }

    // ── Phase 3: Build left-deep operator tree ────────────────────────────────
    // Strategy:
    //   - Start with first alias's scan (possibly wrapped in SelectTranslator)
    //   - For each subsequent alias: create a HashJoin with current plan as probe,
    //     new scan as build
    //   - After all joins: wrap in post-join SelectTranslator if needed

    // Wrap each scan with its filter (if any)
    // We store the operator plan per alias
    std::unordered_map<std::string, OperatorTranslator*> plans;
    for (const std::string& alias : aliases) {
        OperatorTranslator* current = scans[alias];

        if (!filterConjuncts[alias].empty()) {
            // Combine all filter conjuncts into one predicate lambda.
            // Capture the conjuncts by copy (they live in the AST).
            auto conjunctsCopy = filterConjuncts[alias];
            current = new SelectTranslator(ctx, current,
                [this, conjunctsCopy](ConsumerScope& scope) -> SQLValue {
                    // Emit each conjunct and AND them together
                    SQLValue result = evalExpr(conjunctsCopy[0], scope);
                    for (size_t i = 1; i < conjunctsCopy.size(); ++i) {
                        SQLValue next = evalExpr(conjunctsCopy[i], scope);
                        // result AND next: emit LAnd, wrap as Bool SQLValue
                        Bool b = result.asBool() && next.asBool();
                        result = {b.ref, NullRef, SQLType::Bool, &ctx.codegen};
                    }
                    return result;
                });
        }
        plans[alias] = current;
    }

    // Build join tree
    OperatorTranslator* planRoot = plans[aliases[0]];

    if (aliases.size() == 1) {
        // Single table query — planRoot is already set
    } else {
        // For each subsequent alias, find a join key connecting it to what we
        // have so far.  Build left-deep: current plan is probe side, new scan
        // is build side.  (We match the paper: first FROM table = build side.)
        for (size_t i = 1; i < aliases.size(); ++i) {
            const std::string& newAlias = aliases[i];

            // Gather join keys where one side is newAlias
            IUSet buildKeys, probeKeys;
            for (const JoinKey& jk : joinKeys) {
                if (jk.buildAlias == aliases[0] && jk.probeAlias == newAlias) {
                    buildKeys.push_back(jk.buildIU);
                    probeKeys.push_back(jk.probeIU);
                } else if (jk.probeAlias == aliases[0] && jk.buildAlias == newAlias) {
                    // Reversed
                    buildKeys.push_back(jk.probeIU);
                    probeKeys.push_back(jk.buildIU);
                }
            }

            // Build the join (build=first-alias scan side, probe=current plan)
            // Per paper: build side = inner (smaller) table; probe = outer (larger)
            // Here: first alias = build, subsequent alias = probe
            OperatorTranslator* buildPlan = plans[aliases[0]];
            OperatorTranslator* probePlan = plans[newAlias];

            // If we already have a join tree, it becomes the probe side
            if (i == 1) {
                // First join: build=aliases[0] plan, probe=aliases[1] plan
                planRoot = new HashJoinTranslator(ctx,
                    buildPlan, probePlan, buildKeys, probeKeys);
            } else {
                // Further joins: build=new alias scan, probe=current join tree
                // Swap: build=newAlias, probe=planRoot
                // We need to find keys for this new alias
                IUSet bk2, pk2;
                for (const JoinKey& jk : joinKeys) {
                    if (jk.buildAlias == newAlias) {
                        bk2.push_back(jk.buildIU);
                        pk2.push_back(jk.probeIU);
                    } else if (jk.probeAlias == newAlias) {
                        bk2.push_back(jk.probeIU);
                        pk2.push_back(jk.buildIU);
                    }
                }
                planRoot = new HashJoinTranslator(ctx,
                    plans[newAlias], planRoot, bk2, pk2);
            }
        }
    }

    // Wrap with post-join filters if any
    if (!postJoinFilters.empty()) {
        auto pfCopy = postJoinFilters;
        planRoot = new SelectTranslator(ctx, planRoot,
            [this, pfCopy](ConsumerScope& scope) -> SQLValue {
                SQLValue result = evalExpr(pfCopy[0], scope);
                for (size_t i = 1; i < pfCopy.size(); ++i) {
                    SQLValue next = evalExpr(pfCopy[i], scope);
                    Bool b = result.asBool() && next.asBool();
                    result = {b.ref, NullRef, SQLType::Bool, &ctx.codegen};
                }
                return result;
            });
    }

    // ── Phase 4: Resolve SELECT list → output IUs ────────────────────────────
    PlanResult result;
    result.root = planRoot;

    for (const SelectItem& item : stmt.selectList) {
        if (item.isStar) {
            // Expand * → all columns in FROM order
            for (const std::string& alias : aliases) {
                const InMemoryTable* tbl = aliasTables[alias];
                for (const std::string& col : tbl->columnNames) {
                    IU* iu = scans[alias]->getIU(col);
                    result.outputIUs.push_back(iu);
                    result.columnNames.push_back(col);
                }
            }
        } else {
            // Must be a ColumnRef in the SELECT list
            if (item.expr->kind == ExprKind::ColumnRef) {
                IU* iu = resolveColumn(item.expr->colRef);
                result.outputIUs.push_back(iu);
                std::string name = item.alias.empty()
                    ? item.expr->colRef.colName
                    : item.alias;
                result.columnNames.push_back(name);
            } else {
                // For non-column expressions in SELECT we'd need to evaluate
                // them — not supported yet.
                throw std::runtime_error(
                    "Only column references supported in SELECT list");
            }
        }
    }

    return result;
}
