// hash_join_translator.cpp
#include "hash_join_translator.hpp"
#include <algorithm>

// ─── RuntimeHashTable implementation (precompiled, called via proxy) ───────────

void RuntimeHashTable::insert(RuntimeHashTable* ht, uint64_t hash,
                               const int64_t* keys, const int64_t* payload,
                               int numKeys, int numPayload) {
    size_t bucket = hash % NUM_BUCKETS;
    HTEntry entry;
    for (int i = 0; i < numKeys; ++i)    entry.keyData.push_back(keys[i]);
    for (int i = 0; i < numPayload; ++i) entry.payloadData.push_back(payload[i]);
    ht->buckets[bucket].push_back(std::move(entry));
}

RuntimeHashTable* RuntimeHashTable::lookupAll(RuntimeHashTable* ht, uint64_t hash,
                                               const int64_t* probeKeys, int numKeys) {
    size_t bucket = hash % NUM_BUCKETS;
    ht->matchBuffer.clear();
    for (HTEntry& entry : ht->buckets[bucket]) {
        bool match = true;
        for (int i = 0; i < numKeys && match; ++i)
            if (entry.keyData[i] != probeKeys[i]) match = false;
        if (match) ht->matchBuffer.push_back(&entry);
    }
    return ht;
}

int64_t RuntimeHashTable::matchCount(RuntimeHashTable* ht) {
    return static_cast<int64_t>(ht->matchBuffer.size());
}

HTEntry* RuntimeHashTable::matchAt(RuntimeHashTable* ht, int64_t idx) {
    if (idx < 0 || idx >= static_cast<int64_t>(ht->matchBuffer.size()))
        return nullptr;
    return ht->matchBuffer[static_cast<size_t>(idx)];
}

// ─── HashJoinTranslator ─────────────────────────────────────────────────────

HashJoinTranslator::HashJoinTranslator(CompilationContext& ctx,
                                        OperatorTranslator* build,
                                        OperatorTranslator* probe,
                                        IUSet bkeys, IUSet pkeys)
    : OperatorTranslator(ctx),
      buildSide(build), probeSide(probe),
      buildKeys(bkeys), probeKeys(pkeys),
      hashTable(new RuntimeHashTable())
{
    // Build payload IUs = all non-key build IUs
    for (IU* iu : buildSide->outputIUs) {
        bool isKey = false;
        for (IU* k : buildKeys) if (k == iu) { isKey = true; break; }
        if (!isKey) buildPayloadIUs.push_back(iu);
    }

    // Output IUs = probe side all + build key IUs + build payload IUs
    for (IU* iu : probeSide->outputIUs) outputIUs.push_back(iu);
    for (IU* iu : buildKeys)            outputIUs.push_back(iu);  // build keys accessible via payload
    for (IU* iu : buildPayloadIUs)      outputIUs.push_back(iu);
}

void HashJoinTranslator::produce(OperatorTranslator* p) {
    parent = p;
    // Phase 1: build — ask build side to produce; we'll intercept in consume()
    buildSide->produce(this);
    // Phase 2: probe — ask probe side to produce
    probeSide->produce(this);
}

void HashJoinTranslator::consume(ConsumerScope& scope,
                                  OperatorTranslator* child) {
    if (child == buildSide) {
        consumeBuildSide(scope);
    } else {
        consumeProbeSide(scope);
    }
}

void HashJoinTranslator::consumeBuildSide(ConsumerScope& scope) {
    // Paper: "the hash-join operator translator must take each incoming tuple
    //         from the build side and insert it into a hash-table."
    //
    // We emit IR that:
    // 1. Hashes the build keys
    // 2. Packs all key+payload values into a temporary array
    // 3. Calls RuntimeHashTable::insert (proxy call)

    CodegenContext& cg = ctx->codegen;
    IRProgram& prog    = ctx->program;

    // Collect build key SQLValues
    std::vector<SQLValue> keyVals;
    for (IU* k : buildKeys) keyVals.push_back(scope.get(k));

    // Collect payload SQLValues (non-key build IUs)
    std::vector<SQLValue> payloadVals;
    for (IU* p : buildPayloadIUs) payloadVals.push_back(scope.get(p));

    // 1. Hash the keys (paper's hash function)
    UInt64 hash = hashValues(cg, keyVals);

    // 2. Emit call to RuntimeHashTable::insert
    IRValueRef htPtr = prog.addConstInt(
        reinterpret_cast<int64_t>(hashTable), IRType::Ptr);

    int nKeys    = (int)keyVals.size();
    int nPayload = (int)payloadVals.size();

    hashTable->numKeyFields     = nKeys;
    hashTable->numPayloadFields = nPayload;

    // Build call args: (ht, hash, k0..k3, p0..p3) — keys and payloads each padded to 4
    IRValueRef zero4 = prog.addConstInt(0, IRType::Int64);
    std::vector<IRValueRef> callArgs = {htPtr, hash.ref};
    // Keys: up to 4 slots
    for (const SQLValue& kv : keyVals)     callArgs.push_back(kv.valueRef);
    while ((int)callArgs.size() < 2 + 4)   callArgs.push_back(zero4);
    // Payloads: up to 4 slots
    for (const SQLValue& pv : payloadVals) callArgs.push_back(pv.valueRef);
    while ((int)callArgs.size() < 2 + 4 + 4) callArgs.push_back(zero4);

    using InsertFn = void(*)(RuntimeHashTable*, uint64_t,
                              int64_t, int64_t, int64_t, int64_t,
                              int64_t, int64_t, int64_t, int64_t);

    // Maximum 4 keys + 4 payload for this POC
    static InsertFn insertFn = [](RuntimeHashTable* ht, uint64_t hash,
                                   int64_t k0, int64_t k1,
                                   int64_t k2, int64_t k3,
                                   int64_t p0, int64_t p1,
                                   int64_t p2, int64_t p3) {
        int64_t keys[4]    = {k0, k1, k2, k3};
        int64_t payload[4] = {p0, p1, p2, p3};
        RuntimeHashTable::insert(ht, hash, keys, payload,
                                  ht->numKeyFields, ht->numPayloadFields);
    };

    prog.addCall(IRType::Void,
                  reinterpret_cast<uint64_t>(insertFn),
                  callArgs);
}

void HashJoinTranslator::consumeProbeSide(ConsumerScope& scope) {
    // Paper (Section 2.3, Layer 2 DataStructures):
    //   "probe the hash table: look up hash, iterate over chain,
    //    for matching keys: callback with result tuple"

    CodegenContext& cg = ctx->codegen;
    IRProgram& prog    = ctx->program;

    // Collect probe key SQLValues
    std::vector<SQLValue> probeKeyVals;
    for (IU* k : probeKeys) probeKeyVals.push_back(scope.get(k));

    // 1. Hash the probe keys
    UInt64 hash = hashValues(cg, probeKeyVals);

    // 2. Emit call to RuntimeHashTable::lookupAll
    IRValueRef htPtr = prog.addConstInt(
        reinterpret_cast<int64_t>(hashTable), IRType::Ptr);

    int nKeys = (int)probeKeyVals.size();
    hashTable->numKeyFields = nKeys;

    using LookupAllFn = RuntimeHashTable*(*)(RuntimeHashTable*, uint64_t,
                                             int64_t, int64_t, int64_t, int64_t);
    static LookupAllFn lookupAllFn = [](RuntimeHashTable* ht, uint64_t hash,
                                        int64_t k0, int64_t k1,
                                        int64_t k2, int64_t k3) -> RuntimeHashTable* {
        int64_t keys[4] = {k0, k1, k2, k3};
        return RuntimeHashTable::lookupAll(ht, hash, keys, ht->numKeyFields);
    };

    std::vector<IRValueRef> lookupArgs = {htPtr, hash.ref};
    for (const SQLValue& kv : probeKeyVals) lookupArgs.push_back(kv.valueRef);
    while (lookupArgs.size() < 2 + 4) {
        lookupArgs.push_back(prog.addConstInt(0, IRType::Int64));
    }

    IRValueRef matchesPtr = prog.addCall(IRType::Ptr,
                                         reinterpret_cast<uint64_t>(lookupAllFn),
                                         lookupArgs);

    using MatchCountFn = int64_t(*)(RuntimeHashTable*);
    static MatchCountFn matchCountFn = [](RuntimeHashTable* ht) -> int64_t {
        return RuntimeHashTable::matchCount(ht);
    };
    IRValueRef totalMatches = prog.addCall(
        IRType::Int64,
        reinterpret_cast<uint64_t>(matchCountFn),
        {matchesPtr});

    using MatchAtFn = HTEntry*(*)(RuntimeHashTable*, int64_t);
    static MatchAtFn matchAtFn = [](RuntimeHashTable* ht, int64_t idx) -> HTEntry* {
        return RuntimeHashTable::matchAt(ht, idx);
    };

    // 3. Iterate all matches and emit one joined tuple per match.
    IRValueRef zero = prog.addConstInt(0, IRType::Int64);
    IRValueRef one  = prog.addConstInt(1, IRType::Int64);

    uint32_t headerBlk = prog.addBlock("hj_match_header");
    uint32_t bodyBlk   = prog.addBlock("hj_match_body");
    uint32_t exitBlk   = prog.addBlock("hj_match_exit");

    uint32_t entryBlk = prog.currentBlkIdx;
    prog.addBranch(headerBlk);
    prog.setInsertionPoint(prog.currentFnIdx, headerBlk);

    IRValueRef idxPhi = prog.addPhi(IRType::Int64, {{zero, entryBlk}, {NullRef, 0}});
    IRValueRef hasMore = prog.addBinary(Opcode::CmpLt, IRType::Bool, idxPhi, totalMatches);
    prog.addCondBranch(hasMore, bodyBlk, exitBlk);

    prog.setInsertionPoint(prog.currentFnIdx, bodyBlk);

    IRValueRef entryPtr = prog.addCall(
        IRType::Ptr,
        reinterpret_cast<uint64_t>(matchAtFn),
        {matchesPtr, idxPhi});

    // payload is at entry->payloadData.data()[i]
    using GetPayloadFn = int64_t(*)(HTEntry*, int32_t);
    static GetPayloadFn getPayloadFn = [](HTEntry* e, int32_t i) -> int64_t {
        return e->payloadData[i];
    };

    ConsumerScope joinScope;
    joinScope.mergeFrom(scope);  // include probe side values

    // Bind build key IUs by loading from HTEntry::keyData
    using GetKeyFn = int64_t(*)(HTEntry*, int32_t);
    static GetKeyFn getKeyFn = [](HTEntry* e, int32_t i) -> int64_t {
        return e->keyData[i];
    };
    for (size_t i = 0; i < buildKeys.size(); ++i) {
        IRValueRef idxConst = prog.addConstInt((int64_t)i, IRType::Int32);
        IRValueRef keyVal = prog.addCall(
            IRType::Int64,
            reinterpret_cast<uint64_t>(getKeyFn),
            {entryPtr, idxConst});
        joinScope.bind(buildKeys[i],
                       makeNonNullSQLValue(keyVal, buildKeys[i]->type, cg));
    }

    // Bind build payload IUs from HTEntry::payloadData
    for (size_t i = 0; i < buildPayloadIUs.size(); ++i) {
        IRValueRef idxConst = prog.addConstInt((int64_t)i, IRType::Int32);
        IRValueRef payloadVal = prog.addCall(
            IRType::Int64,
            reinterpret_cast<uint64_t>(getPayloadFn),
            {entryPtr, idxConst});
        joinScope.bind(buildPayloadIUs[i],
                       makeNonNullSQLValue(payloadVal,
                                           buildPayloadIUs[i]->type,
                                           cg));
    }

    if (parent) parent->consume(joinScope, this);

    IRValueRef idxNext = prog.addBinary(Opcode::Add, IRType::Int64, idxPhi, one);
    uint32_t backEdgeBlk = prog.currentBlkIdx;
    prog.patchPhiEntry(idxPhi, 1, idxNext, backEdgeBlk);
    prog.addBranch(headerBlk);

    prog.setInsertionPoint(prog.currentFnIdx, exitBlk);
}
