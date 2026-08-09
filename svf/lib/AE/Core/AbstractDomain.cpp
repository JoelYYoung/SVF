//===- AbstractDomain.cpp -- Abstract relational-state API -------------===//

#include "AE/Core/AbstractDomain.h"

#include <stdexcept>
#include <utility>

using namespace relational;

const char* relational::toString(CheckResult result)
{
    switch (result)
    {
    case CheckResult::False:
        return "false";
    case CheckResult::True:
        return "true";
    case CheckResult::Unknown:
        return "unknown";
    }
    return "unknown";
}

AbstractState::AbstractState(Environment environment)
    : environment_(std::move(environment))
{
}

AbstractState::~AbstractState() = default;

void AbstractState::report(OperationKind operation,
                           ApproximationKind approximation,
                           std::string reason) const
{
    DiagnosticSink* sink = diagnosticSink();
    if (sink && approximation != ApproximationKind::Exact &&
        approximation != ApproximationKind::BestAbstraction)
        sink->report({operation, approximation, std::move(reason)});
}

void AbstractState::requireCompatible(const AbstractState& other) const
{
    if (!hasCompatibleDomain(other))
        throw std::invalid_argument(
            "relational states use incompatible domains or configurations");
    if (environment_ != other.environment_)
        throw std::invalid_argument(
            "relational states use different environments");
}

void AbstractState::assign(Variable target,
                           const LinearExpression& expression)
{
    const ApproximationKind approximation = assignState(target, expression);
    report(OperationKind::Assignment, approximation,
           approximation == ApproximationKind::UnsupportedFallback
               ? std::string(name()) +
                     " forgot a target assigned an unsupported linear expression"
               : std::string(name()) +
                     " approximated a linear assignment");
}

void AbstractState::assign(Variable target, const TreeExpression& expression)
{
    if (const auto linear = expression.asLinear())
    {
        assign(target, *linear);
        return;
    }
    forget(target);
    report(OperationKind::Assignment,
           ApproximationKind::UnsupportedFallback,
           std::string(name()) +
               " forgot a target assigned a nonlinear or floating tree expression");
}

void AbstractState::assume(const LinearConstraint& constraint)
{
    const ApproximationKind approximation = assumeState(constraint);
    report(OperationKind::Assumption, approximation,
           std::string(name()) +
               " ignored or approximated an unsupported constraint");
}

void AbstractState::assume(const TreeConstraint& constraint)
{
    if (const auto linear = constraint.expression().asLinear())
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    report(OperationKind::Assumption,
           ApproximationKind::UnsupportedFallback,
           std::string(name()) +
               " ignored a nonlinear or floating assumption");
}

void AbstractState::forget(Variable variable)
{
    forgetState(variable);
}

std::unique_ptr<AbstractState>
AbstractState::joined(const AbstractState& other) const
{
    std::unique_ptr<AbstractState> result = clone();
    result->joinWith(other);
    return result;
}

std::unique_ptr<AbstractState> AbstractState::met(
    const AbstractState& other) const
{
    std::unique_ptr<AbstractState> result = clone();
    result->meetWith(other);
    return result;
}

std::unique_ptr<AbstractState> AbstractState::widened(
    const AbstractState& next, const WideningPolicy& policy) const
{
    std::unique_ptr<AbstractState> result = clone();
    result->widenWith(next, policy);
    return result;
}

std::unique_ptr<AbstractState> AbstractState::narrowed(
    const AbstractState& next) const
{
    std::unique_ptr<AbstractState> result = clone();
    result->narrowWith(next);
    return result;
}

std::unique_ptr<AbstractState> AbstractState::projectLowerBounds() const
{
    std::unique_ptr<AbstractState> result = clone();
    result->projectLowerBoundsState();
    return result;
}

std::unique_ptr<AbstractState> AbstractState::changedEnvironment(
    const Environment& environment, bool projectNewVariables) const
{
    std::unique_ptr<AbstractState> result = clone();
    result->changeEnvironment(environment, projectNewVariables);
    return result;
}

void AbstractState::joinWith(const AbstractState& other)
{
    requireCompatible(other);
    joinState(other);
}

void AbstractState::meetWith(const AbstractState& other)
{
    requireCompatible(other);
    meetState(other);
}

void AbstractState::widenWith(const AbstractState& next,
                              const WideningPolicy& policy)
{
    requireCompatible(next);
    widenState(next, policy);
}

void AbstractState::narrowWith(const AbstractState& next)
{
    requireCompatible(next);
    if (!next.leqState(*this))
        throw std::invalid_argument(
            "narrowing requires next to be included in current");
    narrowState(next);
}

void AbstractState::changeEnvironment(const Environment& environment,
                                      bool projectNewVariables)
{
    const Environment oldEnvironment = environment_;
    changeEnvironmentState(oldEnvironment, environment, projectNewVariables);
    environment_ = environment;
}

bool AbstractState::isBottom() const
{
    return isBottomState();
}

bool AbstractState::isTop() const
{
    return isTopState();
}

CheckResult AbstractState::leq(const AbstractState& other) const
{
    requireCompatible(other);
    return leqState(other) ? CheckResult::True : CheckResult::False;
}

CheckResult AbstractState::equals(const AbstractState& other) const
{
    requireCompatible(other);
    return leqState(other) && other.leqState(*this) ? CheckResult::True
                                                    : CheckResult::False;
}

CheckResult AbstractState::entails(const LinearConstraint& constraint) const
{
    if (constraint.kind() == ConstraintKind::Equal)
    {
        const CheckResult lessOrEqual = entails(LinearConstraint(
            constraint.expression(), ConstraintKind::LessEqual));
        const CheckResult greaterOrEqual = entails(LinearConstraint(
            constraint.expression(), ConstraintKind::GreaterEqual));
        if (lessOrEqual == CheckResult::True &&
            greaterOrEqual == CheckResult::True)
            return CheckResult::True;
        if (lessOrEqual == CheckResult::False ||
            greaterOrEqual == CheckResult::False)
            return CheckResult::False;
        return CheckResult::Unknown;
    }

    if (constraint.kind() == ConstraintKind::NotEqual)
    {
        std::unique_ptr<AbstractState> equalityWitness = clone();
        equalityWitness->assume(
            LinearConstraint(constraint.expression(), ConstraintKind::Equal));
        return equalityWitness->isBottom() ? CheckResult::True
                                           : CheckResult::False;
    }

    std::unique_ptr<AbstractState> counterexample = clone();
    ConstraintKind negated;
    switch (constraint.kind())
    {
    case ConstraintKind::Equal:
    case ConstraintKind::NotEqual:
        throw std::logic_error("equality entailment was not normalized");
    case ConstraintKind::LessThan:
        negated = ConstraintKind::GreaterEqual;
        break;
    case ConstraintKind::LessEqual:
        negated = ConstraintKind::GreaterThan;
        break;
    case ConstraintKind::GreaterThan:
        negated = ConstraintKind::LessEqual;
        break;
    case ConstraintKind::GreaterEqual:
        negated = ConstraintKind::LessThan;
        break;
    }
    counterexample->assume(
        LinearConstraint(constraint.expression(), negated));
    return counterexample->isBottom() ? CheckResult::True
                                      : CheckResult::False;
}

Interval AbstractState::bound(Variable variable) const
{
    return boundState(variable);
}

Box AbstractState::toBox() const
{
    Box box;
    for (const VariableDeclaration& declaration : environment_.variables())
        box.bounds.emplace(declaration.variable, bound(declaration.variable));
    return box;
}

LinearConstraintSet AbstractState::toConstraints() const
{
    return constraintsState();
}

std::string AbstractState::toString() const
{
    return stateToString();
}
