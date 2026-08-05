//===- RelationalDomain.cpp -- APRON-style relational API ---------------===//

#include "AE/Core/RelationalDomain.h"

#include "AE/Core/OctagonDomain.h"

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

Manager::Manager(std::shared_ptr<DomainBackend> backend,
                 std::shared_ptr<DiagnosticSink> diagnostics)
    : backend_(std::move(backend)), diagnostics_(std::move(diagnostics))
{
    if (!backend_)
        throw std::invalid_argument("relational manager requires a backend");
}

Manager::~Manager() = default;

const char* Manager::backendName() const
{
    return backend_->name();
}

DomainCapabilities Manager::capabilities() const
{
    return backend_->capabilities();
}

AbstractState Manager::top(const Environment& environment) const
{
    return AbstractState(shared_from_this(), environment,
                         backend_->top(environment));
}

AbstractState Manager::bottom(const Environment& environment) const
{
    return AbstractState(shared_from_this(), environment,
                         backend_->bottom(environment));
}

AbstractState Manager::fromBox(const Environment& environment,
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
            state.assume(LinearConstraint(
                std::move(expression), interval.lower().isStrict()
                                           ? ConstraintKind::GreaterThan
                                           : ConstraintKind::GreaterEqual));
        }
        if (interval.upper().isFinite())
        {
            LinearExpression expression(variable);
            expression.setConstant(-interval.upper().value());
            state.assume(LinearConstraint(
                std::move(expression), interval.upper().isStrict()
                                           ? ConstraintKind::LessThan
                                           : ConstraintKind::LessEqual));
        }
    }
    return state;
}

AbstractState Manager::fromConstraints(
    const Environment& environment, const ConstraintSet& constraints) const
{
    AbstractState state = top(environment);
    for (const LinearConstraint& constraint : constraints)
        state.assume(constraint);
    return state;
}

void Manager::report(OperationKind operation,
                     ApproximationKind approximation, std::string reason) const
{
    if (diagnostics_ && approximation != ApproximationKind::Exact &&
            approximation != ApproximationKind::BestAbstraction)
        diagnostics_->report({operation, approximation, std::move(reason)});
}

std::shared_ptr<Manager>
relational::makeOctagonManager(const OctagonOptions& options)
{
    return std::shared_ptr<Manager>(
        new Manager(makeOctagonBackend(options), options.diagnostics));
}

AbstractState::AbstractState(std::shared_ptr<const Manager> manager,
                             Environment environment,
                             std::unique_ptr<BackendState> state)
    : manager_(std::move(manager)), environment_(std::move(environment)),
      state_(std::move(state))
{
    if (!manager_ || !state_)
        throw std::invalid_argument(
            "relational state requires a manager and backend state");
}

AbstractState::AbstractState(const AbstractState& rhs)
    : manager_(rhs.manager_), environment_(rhs.environment_),
      state_(rhs.state_->clone())
{
}

AbstractState::AbstractState(AbstractState&& rhs) noexcept = default;

AbstractState& AbstractState::operator=(const AbstractState& rhs)
{
    if (this == &rhs)
        return *this;
    manager_ = rhs.manager_;
    environment_ = rhs.environment_;
    state_ = rhs.state_->clone();
    return *this;
}

AbstractState& AbstractState::operator=(AbstractState&& rhs) noexcept = default;

AbstractState::~AbstractState() = default;

const DomainBackend& AbstractState::backend() const
{
    return *manager_->backend_;
}

DomainBackend& AbstractState::backend()
{
    return *manager_->backend_;
}

void AbstractState::requireCompatible(const AbstractState& other) const
{
    if (manager_->backend_.get() != other.manager_->backend_.get())
        throw std::invalid_argument(
            "relational states use different backend managers");
    if (environment_ != other.environment_)
        throw std::invalid_argument(
            "relational states use different environments");
}

void AbstractState::assign(Variable target,
                           const LinearExpression& expression)
{
    const ApproximationKind approximation =
        backend().assign(*state_, environment_, target, expression);
    manager_->report(OperationKind::Assignment, approximation,
                     approximation == ApproximationKind::UnsupportedFallback
                         ? "Octagon forgot a target assigned an unsupported "
                           "linear expression"
                         : "Octagon approximated a linear assignment");
}

void AbstractState::assign(Variable target,
                           const TreeExpression& expression)
{
    if (const auto linear = expression.asLinear())
    {
        assign(target, *linear);
        return;
    }
    forget(target);
    manager_->report(OperationKind::Assignment,
                     ApproximationKind::UnsupportedFallback,
                     "Octagon forgot a target assigned a nonlinear or "
                     "floating tree expression");
}

void AbstractState::assume(const LinearConstraint& constraint)
{
    const ApproximationKind approximation =
        backend().assume(*state_, environment_, constraint);
    manager_->report(OperationKind::Assumption, approximation,
                     "Octagon ignored or approximated an unsupported "
                     "constraint");
}

void AbstractState::assume(const TreeConstraint& constraint)
{
    if (const auto linear = constraint.expression().asLinear())
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    manager_->report(OperationKind::Assumption,
                     ApproximationKind::UnsupportedFallback,
                     "Octagon ignored a nonlinear or floating assumption");
}

void AbstractState::forget(Variable variable)
{
    backend().forget(*state_, environment_, variable);
}

AbstractState AbstractState::joined(const AbstractState& other) const
{
    requireCompatible(other);
    return AbstractState(manager_, environment_,
                         backend().join(*state_, *other.state_));
}

AbstractState AbstractState::met(const AbstractState& other) const
{
    requireCompatible(other);
    return AbstractState(manager_, environment_,
                         backend().meet(*state_, *other.state_));
}

AbstractState AbstractState::widened(const AbstractState& next,
                                     const WideningPolicy& policy) const
{
    requireCompatible(next);
    return AbstractState(manager_, environment_,
                         backend().widen(*state_, *next.state_, policy));
}

AbstractState AbstractState::narrowed(const AbstractState& next) const
{
    requireCompatible(next);
    if (!backend().leq(*next.state_, *state_))
        throw std::invalid_argument(
            "narrowing requires next to be included in current");
    return AbstractState(manager_, environment_,
                         backend().narrow(*state_, *next.state_));
}

AbstractState AbstractState::projectLowerBounds() const
{
    return AbstractState(manager_, environment_,
                         backend().projectLowerBounds(*state_));
}

AbstractState AbstractState::changedEnvironment(
    const Environment& environment, bool projectNewVariables) const
{
    return AbstractState(
        manager_, environment,
        backend().changeEnvironment(*state_, environment_, environment,
                                    projectNewVariables));
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
    return backend().isBottom(*state_);
}

bool AbstractState::isTop() const
{
    return backend().isTop(*state_);
}

CheckResult AbstractState::leq(const AbstractState& other) const
{
    requireCompatible(other);
    return backend().leq(*state_, *other.state_) ? CheckResult::True
                                                 : CheckResult::False;
}

CheckResult AbstractState::equals(const AbstractState& other) const
{
    requireCompatible(other);
    return backend().leq(*state_, *other.state_) &&
                   backend().leq(*other.state_, *state_)
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
        equalityWitness.assume(LinearConstraint(
            constraint.expression(), ConstraintKind::Equal));
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
    counterexample.assume(
        LinearConstraint(constraint.expression(), negated));
    return counterexample.isBottom() ? CheckResult::True : CheckResult::False;
}

Interval AbstractState::bound(Variable variable) const
{
    return backend().bound(*state_, environment_, variable);
}

Box AbstractState::toBox() const
{
    Box box;
    for (const VariableDeclaration& declaration : environment_.variables())
        box.bounds.emplace(declaration.variable, bound(declaration.variable));
    return box;
}

ConstraintSet AbstractState::toConstraints() const
{
    return backend().constraints(*state_, environment_);
}

std::string AbstractState::toString() const
{
    return backend().toString(*state_, environment_);
}
