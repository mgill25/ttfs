#pragma once
// operator_base.hpp - Base class for operator translators
//
// Implements the produce/consume interface described in the paper:
//   "Operator translators get tuples from their child operators and
//    pass control to each other following the produce/consume interface."
//
// The flow (for a filter on top of a scan):
//   1. Root calls filter.produce(nullptr)
//   2. Filter calls scan.produce(filter)          -- "ask child to produce"
//   3. Scan emits a loop over rows
//   4. For each row: scan calls filter.consume(scope, scan)  -- "here's a tuple"
//   5. Filter checks predicate, if passes: (no parent, so prints result)

#include "compilation_context.hpp"

class OperatorTranslator {
public:
    CompilationContext* ctx = nullptr;

    explicit OperatorTranslator(CompilationContext& c) : ctx(&c) {}
    virtual ~OperatorTranslator() = default;

    // Called top-down: "start producing tuples for parent"
    // parent = nullptr means this is the root (print results)
    virtual void produce(OperatorTranslator* parent) = 0;

    // Called bottom-up: "child has produced a tuple in scope"
    virtual void consume(ConsumerScope& scope, OperatorTranslator* child) = 0;

    // The IUs this operator outputs
    IUSet outputIUs;
};
