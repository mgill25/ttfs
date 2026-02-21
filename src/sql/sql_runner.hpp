#pragma once
// sql_runner.hpp - runQuery() entry point
//
// Ties together lexer → parser → planner → code generation → execution.
// Returns a QueryResult with column names and rows.

#include "../operators/scan_translator.hpp"
#include <string>
#include <vector>
#include <ostream>

// ─── Query result ─────────────────────────────────────────────────────────────
struct QueryResult {
    std::vector<std::string>           columnNames;
    std::vector<std::vector<int64_t>>  rows;
};

// ─── EXPLAIN verbosity flags ──────────────────────────────────────────────────
// Controls which pipeline stages are shown by EXPLAIN.
//
//   tt=1  → Tidy Tuples: operator tree + Umbra IR (before/after DCE)
//   fs=1  → Flying Start: native assembly + compilation statistics
//
// Both can be combined: EXPLAIN tt=1 fs=1 <sql>
// EXPLAIN alone (no flags) shows the operator tree only.
struct ExplainOptions {
    bool showTree = false;   // always on for EXPLAIN
    bool showTT   = false;   // tt=1  : Tidy Tuples / Umbra IR
    bool showFS   = false;   // fs=1  : Flying Start assembly
};

// ─── Entry point ──────────────────────────────────────────────────────────────
// Execute a SQL string against the given tables.
// Tables are registered by name (InMemoryTable::name).
// If explain is non-null, pipeline stages are printed to out instead of
// executing the query (results will be empty).
QueryResult runQuery(const std::string& sql,
                     const std::vector<const InMemoryTable*>& tables,
                     const ExplainOptions* explain = nullptr,
                     std::ostream* out = nullptr);
