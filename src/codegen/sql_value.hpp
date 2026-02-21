#pragma once
// sql_value.hpp - Layer 4 of Tidy Tuples: SQL Values
//
// Wraps a codegen value with NULL-semantics.
// "A SQLValue consists of a NULL indicator, the value, and a SQL type specifier."

#include "codegen_types.hpp"

enum class SQLType {
    Integer,  // 32-bit
    BigInt,   // 64-bit
    Double,
    Varchar,  // simplified: stored as hash for POC
    Bool,
};

inline const char* sqlTypeName(SQLType t) {
    switch (t) {
        case SQLType::Integer: return "INTEGER";
        case SQLType::BigInt:  return "BIGINT";
        case SQLType::Double:  return "DOUBLE";
        case SQLType::Varchar: return "VARCHAR";
        case SQLType::Bool:    return "BOOL";
    }
    return "?";
}

inline IRType sqlTypeToIR(SQLType t) {
    switch (t) {
        case SQLType::Integer: return IRType::Int64;
        case SQLType::BigInt:  return IRType::Int64;
        case SQLType::Double:  return IRType::Double;
        case SQLType::Varchar: return IRType::UInt64;  // stored as hash
        case SQLType::Bool:    return IRType::Bool;
    }
    return IRType::Int64;
}

// ─── SQLValue ─────────────────────────────────────────────────────────────────
struct SQLValue {
    IRValueRef valueRef;  // The actual value (IR value reference)
    IRValueRef nullRef;   // Bool: true = IS NULL (NullRef means definitely not null)
    SQLType    sqlType;
    CodegenContext* ctx;

    bool isNullable() const { return nullRef != NullRef; }

    // Get as Int64 wrapper (for use in comparisons etc.)
    Int64 asInt64() const { return {valueRef, ctx}; }
    UInt64 asUInt64() const { return {valueRef, ctx}; }
    Bool   asBool() const  { return {valueRef, ctx}; }

    // NULL propagation: result is NULL if either operand is NULL
    SQLValue propagateNull(IRValueRef resultRef, SQLType rtype) const;
    static SQLValue propagateNull2(const SQLValue& a, const SQLValue& b,
                                   IRValueRef resultRef, SQLType rtype);

    // ── Arithmetic ─────────────────────────────────────────────────────────
    SQLValue add(const SQLValue& other) const {
        IRValueRef r = ctx->emitBinary(Opcode::Add, IRType::Int64,
                                        valueRef, other.valueRef);
        return propagateNull2(*this, other, r, sqlType);
    }

    SQLValue sub(const SQLValue& other) const {
        IRValueRef r = ctx->emitBinary(Opcode::Sub, IRType::Int64,
                                        valueRef, other.valueRef);
        return propagateNull2(*this, other, r, sqlType);
    }

    // ── Comparisons (return Bool SQLValue) ────────────────────────────────
    SQLValue eq(const SQLValue& other) const {
        IRValueRef r = ctx->emitBinary(Opcode::CmpEq, IRType::Bool,
                                        valueRef, other.valueRef);
        return propagateNull2(*this, other, r, SQLType::Bool);
    }

    SQLValue ne(const SQLValue& other) const {
        IRValueRef r = ctx->emitBinary(Opcode::CmpNe, IRType::Bool,
                                        valueRef, other.valueRef);
        return propagateNull2(*this, other, r, SQLType::Bool);
    }

    SQLValue lt(const SQLValue& other) const {
        IRValueRef r = ctx->emitBinary(Opcode::CmpLt, IRType::Bool,
                                        valueRef, other.valueRef);
        return propagateNull2(*this, other, r, SQLType::Bool);
    }

    SQLValue gt(const SQLValue& other) const {
        IRValueRef r = ctx->emitBinary(Opcode::CmpGt, IRType::Bool,
                                        valueRef, other.valueRef);
        return propagateNull2(*this, other, r, SQLType::Bool);
    }

    SQLValue le(const SQLValue& other) const {
        IRValueRef r = ctx->emitBinary(Opcode::CmpLe, IRType::Bool,
                                        valueRef, other.valueRef);
        return propagateNull2(*this, other, r, SQLType::Bool);
    }

    SQLValue ge(const SQLValue& other) const {
        IRValueRef r = ctx->emitBinary(Opcode::CmpGe, IRType::Bool,
                                        valueRef, other.valueRef);
        return propagateNull2(*this, other, r, SQLType::Bool);
    }

    // ── NULL handling ──────────────────────────────────────────────────────
    SQLValue isNull() const {
        if (!isNullable()) {
            // Definitely not null: emit false constant
            IRValueRef falseRef = ctx->emitConstBool(false);
            return {falseRef, NullRef, SQLType::Bool, ctx};
        }
        return {nullRef, NullRef, SQLType::Bool, ctx};
    }

    SQLValue isNotNull() const {
        auto n = isNull();
        IRValueRef r = ctx->emitUnary(Opcode::LNot, IRType::Bool, n.valueRef);
        return {r, NullRef, SQLType::Bool, ctx};
    }
};

inline SQLValue SQLValue::propagateNull(IRValueRef resultRef,
                                         SQLType rtype) const {
    return {resultRef, nullRef, rtype, ctx};
}

inline SQLValue SQLValue::propagateNull2(const SQLValue& a, const SQLValue& b,
                                          IRValueRef resultRef, SQLType rtype) {
    if (!a.isNullable() && !b.isNullable()) {
        return {resultRef, NullRef, rtype, a.ctx};
    }
    if (!a.isNullable()) {
        return {resultRef, b.nullRef, rtype, a.ctx};
    }
    if (!b.isNullable()) {
        return {resultRef, a.nullRef, rtype, a.ctx};
    }
    // Both nullable: result is null if either is null
    IRValueRef combinedNull = a.ctx->emitBinary(Opcode::LOr, IRType::Bool,
                                                  a.nullRef, b.nullRef);
    return {resultRef, combinedNull, rtype, a.ctx};
}

// ─── Factories ────────────────────────────────────────────────────────────────

inline SQLValue makeIntSQLValue(CodegenContext& ctx, int64_t val) {
    IRValueRef r = ctx.emitConstInt(val, IRType::Int64);
    return {r, NullRef, SQLType::Integer, &ctx};
}

inline SQLValue makeBigIntSQLValue(CodegenContext& ctx, int64_t val) {
    IRValueRef r = ctx.emitConstInt(val, IRType::Int64);
    return {r, NullRef, SQLType::BigInt, &ctx};
}

inline SQLValue makeNonNullSQLValue(IRValueRef valueRef, SQLType t,
                                     CodegenContext& ctx) {
    return {valueRef, NullRef, t, &ctx};
}

inline SQLValue makeNullableSQLValue(IRValueRef valueRef, IRValueRef nullRef,
                                      SQLType t, CodegenContext& ctx) {
    return {valueRef, nullRef, t, &ctx};
}
