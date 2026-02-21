#pragma once
// hash_join_translator.hpp - Hash join operator translator
//
// Implements the paper's HJTranslator example:
//   Build phase: iterate build side, hash keys, insert into hash table
//   Probe phase: iterate probe side, hash probe keys, look up in hash table,
//                for each matching entry: emit join result to parent
//
// The "hash table" is a runtime C++ data structure (proxied in).
// This demonstrates the paper's "proxy system" concept: precompiled code
// called from generated code to reduce code generation time.

#include "operator_base.hpp"
#include <unordered_map>
#include <list>

// Runtime hash table entry (precompiled, called via proxy)
struct HTEntry {
    std::vector<int64_t> keyData;
    std::vector<int64_t> payloadData;
};

// Runtime hash table (the "proxy object" in the paper)
// Generated code calls into this precompiled structure
struct RuntimeHashTable {
    static constexpr size_t NUM_BUCKETS = 1024;
    std::vector<std::list<HTEntry>> buckets;
    int numKeyFields    = 0;
    int numPayloadFields = 0;

    RuntimeHashTable() : buckets(NUM_BUCKETS) {}

    // Called from generated code (via Call IR instruction)
    static void insert(RuntimeHashTable* ht, uint64_t hash,
                       const int64_t* keys, const int64_t* payload,
                       int numKeys, int numPayload);

    // Returns a pointer to the first matching entry chain, or nullptr
    static HTEntry* lookup(RuntimeHashTable* ht, uint64_t hash,
                            const int64_t* probeKeys, int numKeys);
};

class HashJoinTranslator : public OperatorTranslator {
    OperatorTranslator* buildSide;
    OperatorTranslator* probeSide;
    OperatorTranslator* parent = nullptr;

    IUSet buildKeys;   // IUs used as join keys from build side
    IUSet probeKeys;   // IUs used as join keys from probe side

    // Runtime hash table (lives in CompilationContext memory for the query)
    RuntimeHashTable* hashTable;

    // IUs produced by the join (both sides)
    IUSet buildPayloadIUs;  // non-key IUs from build side

public:
    HashJoinTranslator(CompilationContext& ctx,
                       OperatorTranslator* buildSide,
                       OperatorTranslator* probeSide,
                       IUSet buildKeys,
                       IUSet probeKeys);

    ~HashJoinTranslator() { delete hashTable; }

    void produce(OperatorTranslator* parent) override;
    void consume(ConsumerScope& scope, OperatorTranslator* child) override;

    // Introspection for EXPLAIN / operator tree printing
    OperatorTranslator* buildSideOp() const { return buildSide; }
    OperatorTranslator* probeSideOp() const { return probeSide; }
    size_t numBuildKeys() const { return buildKeys.size(); }

private:
    void consumeBuildSide(ConsumerScope& scope);
    void consumeProbeSide(ConsumerScope& scope);
};
