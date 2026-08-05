//===- AbstractDomain.cpp -- Abstract relational-domain API -------------===//

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

AbstractDomain::AbstractDomain(std::shared_ptr<DiagnosticSink> diagnostics)
    : diagnostics_(std::move(diagnostics))
{
}

AbstractDomain::~AbstractDomain() = default;

AbstractState AbstractDomain::top(const Environment& environment) const
{
    return AbstractState(shared_from_this(), environment, makeTop(environment));
}

AbstractState AbstractDomain::bottom(const Environment& environment) const
{
    return AbstractState(shared_from_this(), environment,
                         makeBottom(environment));
}

AbstractState AbstractDomain::fromBox(const Environment& environment,
                                      const Box& box) const
{
    AbstractState state = top(environment);
    for (const auto& [variable, interval] : box.bounds)
    {
        if (!environment.contains(variable))
            throw std::invalid_argument("box contains an unknown variable");
        if (interval.isBottom())
            return bottom(environment);
        if (interval.lower().isFinite())
        {
            LinearExpression expression(variable);
            expression.setConstant(-interval.lower().value());
            state.assume(LinearConstraint(std::move(expression),
                                          interval.lower().isStrict()
                                              ? ConstraintKind::GreaterThan
                                              : ConstraintKind::GreaterEqual));
        }
        if (interval.upper().isFinite())
        {
            LinearExpression expression(variable);
            expression.setConstant(-interval.upper().value());
            state.assume(LinearConstraint(std::move(expression),
                                          interval.upper().isStrict()
                                              ? ConstraintKind::LessThan
                                              : ConstraintKind::LessEqual));
        }
    }
    return state;
}

AbstractState AbstractDomain::fromConstraints(
    const Environment& environment,
    const LinearConstraintSet& constraints) const
{
    AbstractState state = top(environment);
    for (const LinearConstraint& constraint : constraints)
        state.assume(constraint);
    return state;
}

void AbstractDomain::report(OperationKind operation,
                            ApproximationKind approximation,
                            std::string reason) const
{
    if (diagnostics_ && approximation != ApproximationKind::Exact &&
        approximation != ApproximationKind::BestAbstraction)
        diagnostics_->report({operation, approximation, std::move(reason)});
}

AbstractState::AbstractState(std::shared_ptr<const AbstractDomain> domain,
                             Environment environment,
                             std::unique_ptr<DomainState> state)
    : domain_(std::move(domain)), environment_(std::move(environment)),
      state_(std::move(state))
{
    if (!domain_ || !state_)
        throw std::invalid_argument(
            "relational state requires a domain and domain state");
}

AbstractState::AbstractState(const AbstractState& rhs)
    : domain_(rhs.domain_), environment_(rhs.environment_),
      state_(rhs.state_->clone())
{
}

AbstractState::AbstractState(AbstractState&& rhs) noexcept = default;

AbstractState& AbstractState::operator=(const AbstractState& rhs)
{
    if (this == &rhs)
        return *this;
    domain_ = rhs.domain_;
    environment_ = rhs.environment_;
    state_ = rhs.state_->clone();
    return *this;
}

AbstractState& AbstractState::operator=(AbstractState&& rhs) noexcept = default;

AbstractState::~AbstractState() = default;

void AbstractState::requireCompatible(const AbstractState& other) const
{
    if (domain_.get() != other.domain_.get())
        throw std::invalid_argument(
            "relational states belong to different abstract domains");
    if (environment_ != other.environment_)
        throw std::invalid_argument(
            "relational states use different environments");
}

void AbstractState::assign(Variable target, const LinearExpression& expression)
{
    const ApproximationKind approximation =
        implementation().assignState(*state_, environment_, target, expression);
    domain_->report(
        OperationKind::Assignment, approximation,
        approximation == ApproximationKind::UnsupportedFallback
            ? std::string(domain_->name()) +
                  " forgot a target assigned an unsupported linear expression"
            : std::string(domain_->name()) +
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
    domain_->report(OperationKind::Assignment,
                    ApproximationKind::UnsupportedFallback,
                    std::string(domain_->name()) +
                        " forgot a target assigned a nonlinear or floating "
                        "tree expression");
}

void AbstractState::assume(const LinearConstraint& constraint)
{
    const ApproximationKind approximation =
        implementation().assumeState(*state_, environment_, constraint);
    domain_->report(OperationKind::Assumption, approximation,
                    std::string(domain_->name()) +
                        " ignored or approximated an unsupported constraint");
}

void AbstractState::assume(const TreeConstraint& constraint)
{
    if (const auto linear = constraint.expression().asLinear())
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    domain_->report(OperationKind::Assumption,
                    ApproximationKind::UnsupportedFallback,
                    std::string(domain_->name()) +
                        " ignored a nonlinear or floating assumption");
}

void AbstractState::forget(Variable variable)
{
    implementation().forgetState(*state_, environment_, variable);
}

AbstractState AbstractState::joined(const AbstractState& other) const
{
    requireCompatible(other);
    return AbstractState(domain_, environment_,
                         implementation().joinStates(*state_, *other.state_));
}

AbstractState AbstractState::met(const AbstractState& other) const
{
    requireCompatible(other);
    return AbstractState(domain_, environment_,
                         implementation().meetStates(*state_, *other.state_));
}

AbstractState AbstractState::widened(const AbstractState& next,
                                     const WideningPolicy& policy) const
{
    requireCompatible(next);
    return AbstractState(
        domain_, environment_,
        implementation().widenStates(*state_, *next.state_, policy));
}

AbstractState AbstractState::narrowed(const AbstractState& next) const
{
    requireCompatible(next);
    if (!implementation().leqStates(*next.state_, *state_))
        throw std::invalid_argument(
            "narrowing requires next to be included in current");
    return AbstractState(domain_, environment_,
                         implementation().narrowStates(*state_, *next.state_));
}

AbstractState AbstractState::projectLowerBounds() const
{
    return AbstractState(domain_, environment_,
                         implementation().projectLowerBoundsState(*state_));
}

AbstractState AbstractState::changedEnvironment(const Environment& environment,
                                                bool projectNewVariables) const
{
    return AbstractState(
        domain_, environment,
        implementation().changeEnvironmentState(
            *state_, environment_, environment, projectNewVariables));
}

void AbstractState::joinWith(const AbstractState& other)
{
    *this = joined(other);
}

void AbstractState::meetWith(const AbstractState& other)
{
    *this = met(other);
}

void AbstractState::widenWith(const AbstractState& next,
                              const WideningPolicy& policy)
{
    *this = widened(next, policy);
}

void AbstractState::narrowWith(const AbstractState& next)
{
    *this = narrowed(next);
}

void AbstractState::changeEnvironment(const Environment& environment,
                                      bool projectNewVariables)
{
    *this = changedEnvironment(environment, projectNewVariables);
}

bool AbstractState::isBottom() const
{
    return implementation().isBottomState(*state_);
}

bool AbstractState::isTop() const
{
    return implementation().isTopState(*state_);
}

CheckResult AbstractState::leq(const AbstractState& other) const
{
    requireCompatible(other);
    return implementation().leqStates(*state_, *other.state_)
               ? CheckResult::True
               : CheckResult::False;
}

CheckResult AbstractState::equals(const AbstractState& other) const
{
    requireCompatible(other);
    return implementation().leqStates(*state_, *other.state_) &&
                   implementation().leqStates(*other.state_, *state_)
               ? CheckResult::True
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
        AbstractState equalityWitness(*this);
        equalityWitness.assume(
            LinearConstraint(constraint.expression(), ConstraintKind::Equal));
        return equalityWitness.isBottom() ? CheckResult::True
                                          : CheckResult::False;
    }

    AbstractState counterexample(*this);
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
    counterexample.assume(LinearConstraint(constraint.expression(), negated));
    return counterexample.isBottom() ? CheckResult::True : CheckResult::False;
}

Interval AbstractState::bound(Variable variable) const
{
    return implementation().boundState(*state_, environment_, variable);
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
    return implementation().constraintsState(*state_, environment_);
}

std::string AbstractState::toString() const
{
    return implementation().stateToString(*state_, environment_);
}
