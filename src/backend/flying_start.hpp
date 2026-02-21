#pragma once
// flying_start.hpp - Flying Start compilation backend (asmJIT-powered)
//
// Translates Umbra IR → real x86-64 machine code via asmJIT x86::Compiler.
// asmJIT handles register allocation, spilling, and call lowering.
// We implement PHI lowering and comparison-branch fusion on top.

#include "../ir/umbra_ir.hpp"
#include <string>
#include <ostream>

// ─── FlyingStartBackend ───────────────────────────────────────────────────────
class FlyingStartBackend {
public:
    FlyingStartBackend();
    ~FlyingStartBackend();

    // Compile an Umbra IR function → native x86-64 machine code.
    // Returns a callable void(*)() function pointer.
    using QueryFn = void(*)();
    QueryFn compile(const IRProgram& prog, const IRFunction& fn);

    // Print the real asmJIT-generated x86-64 assembly to the stream.
    // Only valid after compile().
    void dumpAssembly(std::ostream& out) const;

    // Execute the JIT-compiled function.
    void execute(QueryFn fn) { if (fn) fn(); }

    // Release JIT memory when done.
    void release(QueryFn fn);

    // Statistics reported by the translator
    int stackSlotsUsed()   const { return stats.stackSlots; }
    int registersUsed()    const { return stats.regsUsed; }
    int fusedComparisons() const { return stats.fused; }
    int lazyAddresses()    const { return stats.lazy; }
    int eliminatedMovs()   const { return stats.elimMovs; }

private:
    // asmJIT runtime (owns JIT memory) — opaque pointer avoids header pollution
    void* rt;  // actually asmjit::JitRuntime*

    // Captured assembly log from StringLogger
    std::string asmLog;

    // Statistics
    struct Stats {
        int stackSlots = 0;
        int regsUsed   = 0;
        int fused      = 0;
        int lazy       = 0;
        int elimMovs   = 0;
    } stats;
};
