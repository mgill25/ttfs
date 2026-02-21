// sql_parser.cpp - Recursive-descent SQL parser

#include "sql_parser.hpp"
#include <stdexcept>

// ─── Parser state ─────────────────────────────────────────────────────────────
class Parser {
    const std::vector<Token>& tokens;
    size_t pos = 0;

public:
    explicit Parser(const std::vector<Token>& toks) : tokens(toks) {}

    // ── Peek / consume ──────────────────────────────────────────────────────
    const Token& peek() const { return tokens[pos]; }

    bool at(TokenKind k) const { return tokens[pos].kind == k; }

    Token consume() {
        Token t = tokens[pos];
        if (t.kind != TokenKind::END_OF_INPUT) ++pos;
        return t;
    }

    Token expect(TokenKind k, const char* msg) {
        if (!at(k))
            throw std::runtime_error(
                std::string(msg) + "; got '" + tokens[pos].text + "'");
        return consume();
    }

    bool tryConsume(TokenKind k) {
        if (at(k)) { consume(); return true; }
        return false;
    }

    // ── Top-level: SELECT ────────────────────────────────────────────────────
    SelectStmt parseSelect() {
        expect(TokenKind::KW_SELECT, "Expected SELECT");

        SelectStmt stmt;

        // SELECT list
        stmt.selectList = parseSelectList();

        // FROM
        expect(TokenKind::KW_FROM, "Expected FROM");
        stmt.fromList = parseFromList();

        // Optional WHERE
        if (tryConsume(TokenKind::KW_WHERE)) {
            stmt.whereExpr = parseOrExpr();
        }

        // Optional LIMIT n
        if (tryConsume(TokenKind::KW_LIMIT)) {
            Token t = expect(TokenKind::INT_LIT, "Expected integer after LIMIT");
            if (t.intVal < 0)
                throw std::runtime_error("LIMIT value must be non-negative");
            stmt.limit = t.intVal;
        }

        // Optional trailing semicolon
        tryConsume(TokenKind::SEMICOLON);

        if (!at(TokenKind::END_OF_INPUT))
            throw std::runtime_error(
                "Unexpected token after query: '" + peek().text + "'");

        return stmt;
    }

private:
    // ── SELECT list: * | expr [AS alias] (, expr [AS alias])* ────────────────
    std::vector<SelectItem> parseSelectList() {
        std::vector<SelectItem> items;

        // Handle SELECT *
        if (at(TokenKind::STAR)) {
            consume();
            SelectItem item;
            item.isStar = true;
            items.push_back(std::move(item));
            return items;
        }

        do {
            SelectItem item;
            item.isStar = false;
            item.expr   = parseOrExpr();
            // Optional alias
            if (tryConsume(TokenKind::KW_AS)) {
                item.alias = expect(TokenKind::IDENT, "Expected alias").text;
            }
            items.push_back(std::move(item));
        } while (tryConsume(TokenKind::COMMA));

        return items;
    }

    // ── FROM list: tableName [alias] (, tableName [alias])* ──────────────────
    std::vector<TableRef> parseFromList() {
        std::vector<TableRef> refs;

        do {
            TableRef ref;
            ref.tableName = expect(TokenKind::IDENT, "Expected table name").text;
            // Optional alias (no AS keyword — just IDENT after table name)
            if (at(TokenKind::IDENT)) {
                ref.alias = consume().text;
            } else {
                ref.alias = ref.tableName;
            }
            refs.push_back(std::move(ref));
        } while (tryConsume(TokenKind::COMMA));

        return refs;
    }

    // ── Expression grammar ────────────────────────────────────────────────────
    // orExpr := andExpr ('OR' andExpr)*
    std::unique_ptr<Expr> parseOrExpr() {
        auto lhs = parseAndExpr();
        while (at(TokenKind::KW_OR)) {
            consume();
            auto rhs = parseAndExpr();
            lhs = makeBinOp(BinOpKind::Or, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    // andExpr := compExpr ('AND' compExpr)*
    std::unique_ptr<Expr> parseAndExpr() {
        auto lhs = parseCompExpr();
        while (at(TokenKind::KW_AND)) {
            consume();
            auto rhs = parseCompExpr();
            lhs = makeBinOp(BinOpKind::And, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    // compExpr := addExpr [('='|'<>'|'<'|'>'|'<='|'>=') addExpr]
    std::unique_ptr<Expr> parseCompExpr() {
        auto lhs = parseAddExpr();
        BinOpKind op;
        switch (peek().kind) {
        case TokenKind::EQ:  op = BinOpKind::Eq; break;
        case TokenKind::NE:  op = BinOpKind::Ne; break;
        case TokenKind::LT:  op = BinOpKind::Lt; break;
        case TokenKind::GT:  op = BinOpKind::Gt; break;
        case TokenKind::LE:  op = BinOpKind::Le; break;
        case TokenKind::GE:  op = BinOpKind::Ge; break;
        default: return lhs;
        }
        consume();
        auto rhs = parseAddExpr();
        return makeBinOp(op, std::move(lhs), std::move(rhs));
    }

    // addExpr := primaryExpr [('+' | '-') primaryExpr]*
    std::unique_ptr<Expr> parseAddExpr() {
        auto lhs = parsePrimary();
        while (at(TokenKind::PLUS) || at(TokenKind::MINUS)) {
            BinOpKind op = at(TokenKind::PLUS) ? BinOpKind::Add : BinOpKind::Sub;
            consume();
            auto rhs = parsePrimary();
            lhs = makeBinOp(op, std::move(lhs), std::move(rhs));
        }
        return lhs;
    }

    // primaryExpr := INT_LIT
    //             | IDENT ['.' IDENT]
    //             | '(' orExpr ')'
    std::unique_ptr<Expr> parsePrimary() {
        if (at(TokenKind::INT_LIT)) {
            Token t = consume();
            return makeIntLit(t.intVal);
        }

        if (at(TokenKind::IDENT)) {
            Token first = consume();
            if (tryConsume(TokenKind::DOT)) {
                // qualified: alias.col
                Token col = expect(TokenKind::IDENT, "Expected column name after '.'");
                return makeColumnRef(first.text, col.text);
            }
            // unqualified column
            return makeColumnRef("", first.text);
        }

        if (at(TokenKind::LPAREN)) {
            consume();
            auto e = parseOrExpr();
            expect(TokenKind::RPAREN, "Expected ')'");
            return e;
        }

        throw std::runtime_error(
            "Unexpected token in expression: '" + peek().text + "'");
    }
};

// ─── Public entry point ───────────────────────────────────────────────────────
SelectStmt parseSQL(const std::string& sql) {
    auto tokens = tokenize(sql);
    Parser parser(tokens);
    return parser.parseSelect();
}
