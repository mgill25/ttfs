#pragma once
// sql_lexer.hpp - Token types for the SQL lexer

#include <string>
#include <cstdint>

enum class TokenKind {
    // Keywords
    KW_SELECT,
    KW_FROM,
    KW_WHERE,
    KW_AND,
    KW_OR,
    KW_NOT,
    KW_AS,
    KW_LIMIT,

    // Identifiers and literals
    IDENT,       // table/column names
    INT_LIT,     // integer literal

    // Punctuation
    COMMA,       // ,
    DOT,         // .
    STAR,        // *
    LPAREN,      // (
    RPAREN,      // )

    // Comparison operators
    EQ,          // =
    NE,          // <>
    LT,          // <
    GT,          // >
    LE,          // <=
    GE,          // >=

    // Arithmetic
    PLUS,        // +
    MINUS,       // -

    // Special
    SEMICOLON,   // ;
    END_OF_INPUT,
    UNKNOWN,
};

struct Token {
    TokenKind   kind;
    std::string text;    // raw text (for IDENT)
    int64_t     intVal;  // parsed value (for INT_LIT)

    Token(TokenKind k, std::string t = "", int64_t v = 0)
        : kind(k), text(std::move(t)), intVal(v) {}
};

#include <vector>

// Tokenize a SQL string into a flat token vector.
std::vector<Token> tokenize(const std::string& sql);
