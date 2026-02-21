#pragma once
// sql_parser.hpp - Recursive-descent SQL parser
//
// Parses the token stream produced by the lexer into a SelectStmt AST.
// Supports:
//   SELECT <list> FROM <tables> [WHERE <expr>]
//
// Operator precedence (low → high):
//   orExpr, andExpr, comparisonExpr, addExpr, primaryExpr

#include "sql_ast.hpp"
#include "sql_lexer.hpp"
#include <vector>
#include <memory>

// Parse a SQL string into a SelectStmt.
// Throws std::runtime_error on syntax errors.
SelectStmt parseSQL(const std::string& sql);
