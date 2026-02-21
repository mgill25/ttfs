#pragma once
// tuple_ops.hpp - Layer 3 of Tidy Tuples: Tuple operations
//
// Provides tuple hashing (matching the paper's CRC32 example),
// packing (storing values into memory), and unpacking (loading from memory).

#include "sql_value.hpp"
#include <vector>

// ─── Hash generation ─────────────────────────────────────────────────────────
// Matches the paper's hash generation code (Section 2.5):
//
//   UInt64 hash1(6763793487589347598);
//   UInt64 hash2(4593845798347983834);
//   for each value:
//     hash1 = hash1.crc32(v); hash2 = hash2.crc32(v);
//   UInt64 hash = hash1 ^ hash2.rotateRight(32);
//   hash *= 11400714819323198485;
//
// This generates IR instructions like the paper's figure:
//   %1 = zext i64 %int1
//   %2 = zext i64 %int2
//   %v = or i64 %1, ...
//   %5 = crc32 i64 6763793487589347598, %v
//   ...
inline UInt64 hashValues(CodegenContext& ctx,
                          const std::vector<SQLValue>& values) {
    UInt64 hash1 = makeUInt64(ctx, 6763793487589347598ULL);
    UInt64 hash2 = makeUInt64(ctx, 4593845798347983834ULL);

    for (const SQLValue& sv : values) {
        // Cast value to UInt64 for hashing
        IRValueRef valRef = ctx.prog.addBinary(Opcode::ZExt, IRType::UInt64,
                                                sv.valueRef, sv.valueRef);
        // Actually ZExt takes 1 arg, use the unary version
        valRef = ctx.prog.addUnary(Opcode::ZExt, IRType::UInt64, sv.valueRef);
        UInt64 v{valRef, &ctx};
        hash1 = hash1.crc32(v);
        hash2 = hash2.crc32(v);
    }

    UInt64 hash = hash1 ^ hash2.rotateRight(32);
    UInt64 multiplier = makeUInt64(ctx, 11400714819323198485ULL);
    hash = hash * multiplier;
    return hash;
}

// ─── Tuple layout ─────────────────────────────────────────────────────────────
// Describes how a tuple's fields are packed into a contiguous memory region.
struct FieldLayout {
    int     byteOffset;
    IRType  irType;
    SQLType sqlType;
};

struct TupleLayout {
    std::vector<FieldLayout> fields;
    int nullBitmapOffset;  // offset of null bitmap within tuple
    int totalSize;         // total bytes per tuple (aligned)

    static TupleLayout build(const std::vector<SQLType>& types) {
        TupleLayout layout;
        int offset = 0;

        // Null bitmap first (1 byte covers up to 8 fields)
        layout.nullBitmapOffset = offset;
        int numNullable = (int)types.size();
        offset += (numNullable + 7) / 8;

        // Align to 8 bytes
        if (offset % 8 != 0) offset += 8 - (offset % 8);

        for (SQLType t : types) {
            FieldLayout f;
            f.byteOffset = offset;
            f.irType     = sqlTypeToIR(t);
            f.sqlType    = t;
            layout.fields.push_back(f);
            // All fields are 8 bytes for simplicity
            offset += 8;
        }

        layout.totalSize = offset;
        return layout;
    }
};

// ─── Pack / Unpack ────────────────────────────────────────────────────────────
// Pack: store a vector of SQLValues into a memory region at `base` pointer.
// Generates Store IR instructions.
inline void packTuple(CodegenContext& ctx,
                       IRValueRef base,  // ptr to target memory
                       const TupleLayout& layout,
                       const std::vector<SQLValue>& values) {
    assert(values.size() == layout.fields.size());

    // Write null bitmap (simplified: just 0 or 1 per byte for clarity)
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i].isNullable()) {
            IRValueRef nullBytePtr =
                ctx.emitGEP(base, layout.nullBitmapOffset + (int)i);
            // Store null flag as 0/1 byte (store the Bool value)
            ctx.emitStore(nullBytePtr, values[i].nullRef);
        }
    }

    // Write each field value
    for (size_t i = 0; i < values.size(); ++i) {
        const FieldLayout& fl = layout.fields[i];
        IRValueRef fieldPtr = ctx.emitGEP(base, fl.byteOffset);

        // For nullable fields: only store if not null (simplified: always store)
        ctx.emitStore(fieldPtr, values[i].valueRef);
    }
}

// Unpack: load a vector of SQLValues from a memory region.
// Generates Load IR instructions.
inline std::vector<SQLValue> unpackTuple(CodegenContext& ctx,
                                          IRValueRef base,
                                          const TupleLayout& layout) {
    std::vector<SQLValue> result;

    for (size_t i = 0; i < layout.fields.size(); ++i) {
        const FieldLayout& fl = layout.fields[i];
        IRValueRef fieldPtr = ctx.emitGEP(base, fl.byteOffset);
        IRValueRef val      = ctx.emitLoad(fl.irType, fieldPtr);

        // Load null flag (simplified: per-field byte in null bitmap)
        IRValueRef nullBitmapPtr =
            ctx.emitGEP(base, layout.nullBitmapOffset + (int)i);
        IRValueRef nullFlag = ctx.emitLoad(IRType::Bool, nullBitmapPtr);

        result.push_back(makeNullableSQLValue(val, nullFlag, fl.sqlType, ctx));
    }

    return result;
}
