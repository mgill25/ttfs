#pragma once
// compilation_context.hpp - Compilation context and IU (Informational Unit) management
//
// An IU represents one column flowing between operators.
// ConsumerScope holds the mapping of live IU → SQLValue at a given point.

#include "../codegen/tuple_ops.hpp"
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>

// ─── Informational Unit (IU) ─────────────────────────────────────────────────
// Represents a named column with a SQL type.
// Each IU has a unique ID.
struct IU {
    std::string name;
    SQLType     type;
    uint32_t    id;
};

using IUSet = std::vector<IU*>;

// ─── ConsumerScope ────────────────────────────────────────────────────────────
// Passed from produce/consume calls; maps live IUs to their current IR values.
class ConsumerScope {
    std::unordered_map<uint32_t, SQLValue> bindings;  // IU id → SQLValue

public:
    bool contains(const IU* iu) const {
        return bindings.count(iu->id) > 0;
    }

    SQLValue get(const IU* iu) const {
        auto it = bindings.find(iu->id);
        if (it == bindings.end())
            throw std::runtime_error("IU not in scope: " + iu->name);
        return it->second;
    }

    void bind(const IU* iu, SQLValue val) {
        bindings[iu->id] = val;
    }

    void unbind(const IU* iu) {
        bindings.erase(iu->id);
    }

    // Copy all bindings from another scope (used to merge parent scope)
    void mergeFrom(const ConsumerScope& other) {
        for (auto& [id, val] : other.bindings)
            bindings[id] = val;
    }

    // Emit equality test between two sets of SQLValues.
    // Returns a Bool IR value: true if all match.
    static Bool testValuesEq(CodegenContext& ctx,
                              const std::vector<SQLValue>& a,
                              const std::vector<SQLValue>& b) {
        assert(a.size() == b.size());
        if (a.empty()) return makeBool(ctx, true);

        Bool result = a[0].eq(b[0]).asBool();
        for (size_t i = 1; i < a.size(); ++i) {
            Bool eq = a[i].eq(b[i]).asBool();
            result = result && eq;
        }
        return result;
    }
};

// ─── CompilationContext ───────────────────────────────────────────────────────
// Passed everywhere; owns the IRProgram and IU registry.
class CompilationContext {
    std::vector<std::unique_ptr<IU>> iuStorage;
    uint32_t nextIUId = 0;

public:
    IRProgram    program;
    CodegenContext codegen{program};

    IU* createIU(const std::string& name, SQLType type) {
        auto iu = std::make_unique<IU>();
        iu->name = name;
        iu->type = type;
        iu->id   = nextIUId++;
        IU* ptr  = iu.get();
        iuStorage.push_back(std::move(iu));
        return ptr;
    }
};
