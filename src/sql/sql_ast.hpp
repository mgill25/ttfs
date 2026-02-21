#pragma once
// sql_ast.hpp - SQL AST node types (header-only, tagged union approach)
//
// Represents the output of the SQL parser. Uses tagged unions (not virtual
// dispatch) to avoid vtable overhead and keep the planner simple.

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// ─── Expression Kinds ─────────────────────────────────────────────────────────
enum class ExprKind {
    ColumnRef,    // table.col or just col
    IntLiteral,   // 42
    BinOp,        // lhs op rhs
};

enum class BinOpKind {
    Eq,   // =
    Ne,   // <>
    Lt,   // <
    Gt,   // >
    Le,   // <=
    Ge,   // >=
    Add,  // +
    Sub,  // -
    And,  // AND
    Or,   // OR
};

struct Expr;

struct ColumnRef {
    std::string tableAlias;  // empty if unqualified
    std::string colName;
};

struct BinOpExpr {
    BinOpKind op;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

struct Expr {
    ExprKind kind;
    // Only one of the following is active (based on kind):
    ColumnRef   colRef;    // ExprKind::ColumnRef
    int64_t     intVal{0}; // ExprKind::IntLiteral
    std::unique_ptr<BinOpExpr> binOp; // ExprKind::BinOp
};

// ─── Select Item ──────────────────────────────────────────────────────────────
// Represents one element in the SELECT list.
struct SelectItem {
    bool isStar = false;             // SELECT *
    std::unique_ptr<Expr> expr;      // the expression
    std::string alias;               // AS alias (may be empty)
};

// ─── FROM item (table reference with optional alias) ──────────────────────────
struct TableRef {
    std::string tableName;
    std::string alias;      // may equal tableName if no alias given
};

// ─── Top-level SELECT statement ───────────────────────────────────────────────
struct SelectStmt {
    std::vector<SelectItem> selectList;
    std::vector<TableRef>   fromList;
    std::unique_ptr<Expr>   whereExpr;  // may be null (no WHERE)
    int64_t                 limit = -1; // -1 = no limit
};

// ─── Helpers for building Expr nodes ─────────────────────────────────────────

inline std::unique_ptr<Expr> makeColumnRef(const std::string& alias,
                                            const std::string& col) {
    auto e = std::make_unique<Expr>();
    e->kind = ExprKind::ColumnRef;
    e->colRef.tableAlias = alias;
    e->colRef.colName    = col;
    return e;
}

inline std::unique_ptr<Expr> makeIntLit(int64_t val) {
    auto e = std::make_unique<Expr>();
    e->kind   = ExprKind::IntLiteral;
    e->intVal = val;
    return e;
}

inline std::unique_ptr<Expr> makeBinOp(BinOpKind op,
                                        std::unique_ptr<Expr> lhs,
                                        std::unique_ptr<Expr> rhs) {
    auto e = std::make_unique<Expr>();
    e->kind   = ExprKind::BinOp;
    e->binOp  = std::make_unique<BinOpExpr>();
    e->binOp->op    = op;
    e->binOp->left  = std::move(lhs);
    e->binOp->right = std::move(rhs);
    return e;
}
