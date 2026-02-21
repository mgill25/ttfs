// select_translator.cpp
#include "select_translator.hpp"

SelectTranslator::SelectTranslator(CompilationContext& ctx,
                                    OperatorTranslator* child,
                                    std::function<SQLValue(ConsumerScope&)> pred)
    : OperatorTranslator(ctx), child(child), predicate(std::move(pred))
{
    outputIUs = child->outputIUs;
}

void SelectTranslator::produce(OperatorTranslator* p) {
    parent = p;
    // Just ask child to produce; we'll intercept in consume()
    child->produce(this);
}

void SelectTranslator::consume(ConsumerScope& scope, OperatorTranslator* /*c*/) {
    // Evaluate predicate in current scope
    SQLValue cond = predicate(scope);

    // Emit: if (cond) { parent->consume(scope) }
    // This is the core of the produce/consume model
    Bool condBool = cond.asBool();

    // If the condition is a compile-time constant true, skip the branch
    if (ctx->program.isConstant(condBool.ref) &&
        ctx->program.constIntValue(condBool.ref) != 0) {
        // Always true: just pass through
        if (parent) parent->consume(scope, this);
        return;
    }

    // Emit conditional branch
    IfStmt ifStmt(ctx->codegen, condBool);
    {
        // True branch: pass to parent
        if (parent) parent->consume(scope, this);
    }
    // IfStmt destructor emits merge block
}
