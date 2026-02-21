#pragma once
// select_translator.hpp - Filter/Select operator translator
//
// Wraps a child operator with a predicate. Matches paper description:
//   consume() emits: if (predicate(scope)) { parent->consume(scope) }

#include "operator_base.hpp"
#include <functional>

class SelectTranslator : public OperatorTranslator {
    OperatorTranslator* child;
    OperatorTranslator* parent = nullptr;
    // Predicate: given current scope, emit IR for the filter condition
    std::function<SQLValue(ConsumerScope&)> predicate;

public:
    SelectTranslator(CompilationContext& ctx,
                     OperatorTranslator* child,
                     std::function<SQLValue(ConsumerScope&)> pred);

    void produce(OperatorTranslator* parent) override;
    void consume(ConsumerScope& scope, OperatorTranslator* child) override;

    // Introspection for EXPLAIN / operator tree printing
    OperatorTranslator* childOp() const { return child; }
};
