// sql_lexer.cpp - Hand-written SQL tokenizer
//
// Produces a flat vector of Tokens from a SQL string.
// No external dependencies.

#include "sql_lexer.hpp"
#include <vector>
#include <stdexcept>
#include <cctype>
#include <algorithm>
#include <string>

// ─── Keyword table ────────────────────────────────────────────────────────────
static TokenKind lookupKeyword(const std::string& upper) {
    if (upper == "SELECT") return TokenKind::KW_SELECT;
    if (upper == "FROM")   return TokenKind::KW_FROM;
    if (upper == "WHERE")  return TokenKind::KW_WHERE;
    if (upper == "AND")    return TokenKind::KW_AND;
    if (upper == "OR")     return TokenKind::KW_OR;
    if (upper == "NOT")    return TokenKind::KW_NOT;
    if (upper == "AS")     return TokenKind::KW_AS;
    if (upper == "LIMIT")  return TokenKind::KW_LIMIT;
    return TokenKind::IDENT;
}

// ─── Tokenize ─────────────────────────────────────────────────────────────────
std::vector<Token> tokenize(const std::string& sql) {
    std::vector<Token> tokens;
    size_t i = 0;
    const size_t n = sql.size();

    while (i < n) {
        // Skip whitespace
        if (std::isspace((unsigned char)sql[i])) {
            ++i;
            continue;
        }

        char c = sql[i];

        // Integer literal
        if (std::isdigit((unsigned char)c)) {
            size_t start = i;
            while (i < n && std::isdigit((unsigned char)sql[i])) ++i;
            std::string numStr = sql.substr(start, i - start);
            int64_t val = std::stoll(numStr);
            tokens.emplace_back(TokenKind::INT_LIT, numStr, val);
            continue;
        }

        // Identifier or keyword
        if (std::isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            while (i < n && (std::isalnum((unsigned char)sql[i]) || sql[i] == '_'))
                ++i;
            std::string word = sql.substr(start, i - start);
            // Uppercase for keyword comparison
            std::string upper = word;
            for (char& ch : upper) ch = (char)std::toupper((unsigned char)ch);
            TokenKind kw = lookupKeyword(upper);
            if (kw != TokenKind::IDENT)
                tokens.emplace_back(kw, word, 0);
            else
                tokens.emplace_back(TokenKind::IDENT, word, 0);
            continue;
        }

        // Operators and punctuation
        switch (c) {
        case ',': tokens.emplace_back(TokenKind::COMMA,  ",");  ++i; break;
        case '.': tokens.emplace_back(TokenKind::DOT,    ".");  ++i; break;
        case '*': tokens.emplace_back(TokenKind::STAR,   "*");  ++i; break;
        case '(': tokens.emplace_back(TokenKind::LPAREN, "(");  ++i; break;
        case ')': tokens.emplace_back(TokenKind::RPAREN, ")");  ++i; break;
        case '+': tokens.emplace_back(TokenKind::PLUS,   "+");  ++i; break;
        case '-': tokens.emplace_back(TokenKind::MINUS,     "-");  ++i; break;
        case ';': tokens.emplace_back(TokenKind::SEMICOLON, ";"); ++i; break;
        case '=': tokens.emplace_back(TokenKind::EQ,     "=");  ++i; break;
        case '>':
            if (i + 1 < n && sql[i+1] == '=') {
                tokens.emplace_back(TokenKind::GE, ">="); i += 2;
            } else {
                tokens.emplace_back(TokenKind::GT, ">");  ++i;
            }
            break;
        case '<':
            if (i + 1 < n && sql[i+1] == '>') {
                tokens.emplace_back(TokenKind::NE, "<>"); i += 2;
            } else if (i + 1 < n && sql[i+1] == '=') {
                tokens.emplace_back(TokenKind::LE, "<="); i += 2;
            } else {
                tokens.emplace_back(TokenKind::LT, "<");  ++i;
            }
            break;
        default:
            throw std::runtime_error(
                std::string("Unexpected character in SQL: '") + c + "'");
        }
    }

    tokens.emplace_back(TokenKind::END_OF_INPUT, "");
    return tokens;
}
