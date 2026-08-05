//===- AbstractDomain.h -- Abstract relational-domain API ------*- C++ -*-===//

#ifndef RELATIONAL_ABSTRACT_DOMAIN_H
#define RELATIONAL_ABSTRACT_DOMAIN_H

#include "AE/Core/LinearConstraint.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace relational
{

class DomainState;
class AbstractDomain;
class AbstractState;

enum class CheckResult
{
    False,
    True,
    Unknown
};

const char* toString(CheckResult result);

enum class ApproximationKind
{
    Exact,
    BestAbstraction,
    SoundOverApproximation,
    UnsupportedFallback
};

enum class OperationKind
{
    Assignment,
    Assumption,
    Join,
    Meet,
    Widening,
    Narrowing,
    Conversion
};

struct Diagnostic
{
    OperationKind operation;
    ApproximationKind approximation;
    std::string reason;
};

class DiagnosticSink
{
public:
    virtual ~DiagnosticSink() = default;
    virtual void report(const Diagnostic& diagnostic) = 0;
};

struct DomainCapabilities
{
    bool strictInequalities = false;
    bool integerTightening = false;
    bool thresholdWidening = false;
    bool narrowing = false;
    bool treeExpressions = false;
};

struct WideningPolicy
{
    /// Constants for normalized +/-x +/-y <= c and +/-x <= c templates.
    std::vector<Rational> thresholds;
};

struct Box
{
    std::map<Variable, Interval> bounds;
};

enum class ConversionQuality
{
    Exact,
    BestAbstraction,
    SoundButLossy
};

/// Type-erased internal representation of one element of an abstract domain.
/// Clients manipulate the value-semantic AbstractState facade instead.
class DomainState
{
public:
    virtual ~DomainState() = default;
    virtual std::unique_ptr<DomainState> clone() const = 0;
};

/// Common interface implemented by concrete relational domains.
///
/// An AbstractDomain owns the algorithms and configuration for a family of
/// AbstractState values.  Concrete domains such as OctagonDomain implement
/// the representation-specific lattice and transfer operations below.
class AbstractDomain : public std::enable_shared_from_this<AbstractDomain>
{
public:
    virtual ~AbstractDomain();

    virtual const char* name() const = 0;
    virtual DomainCapabilities capabilities() const = 0;

    AbstractState top(const Environment& environment) const;
    AbstractState bottom(const Environment& environment) const;
    AbstractState fromBox(const Environment& environment, const Box& box) const;
    AbstractState fromConstraints(const Environment& environment,
                                  const LinearConstraintSet& constraints) const;

protected:
    explicit AbstractDomain(std::shared_ptr<DiagnosticSink> diagnostics = {});

private:
    friend class AbstractState;

    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason) const;

    virtual std::unique_ptr<DomainState> makeTop(
        const Environment& environment) const = 0;
    virtual std::unique_ptr<DomainState> makeBottom(
        const Environment& environment) const = 0;

    virtual ApproximationKind assignState(
        DomainState& state, const Environment& environment, Variable target,
        const LinearExpression& expression) const = 0;
    virtual ApproximationKind assumeState(
        DomainState& state, const Environment& environment,
        const LinearConstraint& constraint) const = 0;
    virtual void forgetState(DomainState& state, const Environment& environment,
                             Variable variable) const = 0;

    virtual std::unique_ptr<DomainState> joinStates(
        const DomainState& lhs, const DomainState& rhs) const = 0;
    virtual std::unique_ptr<DomainState> meetStates(
        const DomainState& lhs, const DomainState& rhs) const = 0;
    virtual std::unique_ptr<DomainState> widenStates(
        const DomainState& current, const DomainState& next,
        const WideningPolicy& policy) const = 0;
    virtual std::unique_ptr<DomainState> narrowStates(
        const DomainState& current, const DomainState& next) const = 0;
    virtual std::unique_ptr<DomainState> projectLowerBoundsState(
        const DomainState& state) const = 0;
    virtual std::unique_ptr<DomainState> changeEnvironmentState(
        const DomainState& state, const Environment& oldEnvironment,
        const Environment& newEnvironment, bool projectNewVariables) const = 0;

    virtual bool isBottomState(const DomainState& state) const = 0;
    virtual bool isTopState(const DomainState& state) const = 0;
    virtual bool leqStates(const DomainState& lhs,
                           const DomainState& rhs) const = 0;
    virtual Interval boundState(const DomainState& state,
                                const Environment& environment,
                                Variable variable) const = 0;
    virtual LinearConstraintSet constraintsState(
        const DomainState& state, const Environment& environment) const = 0;
    virtual std::string stateToString(const DomainState& state,
                                      const Environment& environment) const = 0;

    std::shared_ptr<DiagnosticSink> diagnostics_;
};

/// Copyable value facade for one element of a concrete AbstractDomain.
class AbstractState
{
public:
    AbstractState(const AbstractState& rhs);
    AbstractState(AbstractState&& rhs) noexcept;
    AbstractState& operator=(const AbstractState& rhs);
    AbstractState& operator=(AbstractState&& rhs) noexcept;
    ~AbstractState();

    const Environment& environment() const
    {
        return environment_;
    }
    const std::shared_ptr<const AbstractDomain>& domain() const
    {
        return domain_;
    }

    void assign(Variable target, const LinearExpression& expression);
    void assign(Variable target, const TreeExpression& expression);
    void assume(const LinearConstraint& constraint);
    void assume(const TreeConstraint& constraint);
    void forget(Variable variable);

    AbstractState joined(const AbstractState& other) const;
    AbstractState met(const AbstractState& other) const;
    AbstractState widened(const AbstractState& next,
                          const WideningPolicy& policy = {}) const;
    AbstractState narrowed(const AbstractState& next) const;
    AbstractState projectLowerBounds() const;
    AbstractState changedEnvironment(const Environment& environment,
                                     bool projectNewVariables = false) const;

    void joinWith(const AbstractState& other);
    void meetWith(const AbstractState& other);
    void widenWith(const AbstractState& next,
                   const WideningPolicy& policy = {});
    void narrowWith(const AbstractState& next);
    void changeEnvironment(const Environment& environment,
                           bool projectNewVariables = false);

    bool isBottom() const;
    bool isTop() const;
    CheckResult leq(const AbstractState& other) const;
    CheckResult equals(const AbstractState& other) const;
    CheckResult entails(const LinearConstraint& constraint) const;

    Interval bound(Variable variable) const;
    Box toBox() const;
    LinearConstraintSet toConstraints() const;
    std::string toString() const;

private:
    friend class AbstractDomain;
    AbstractState(std::shared_ptr<const AbstractDomain> domain,
                  Environment environment, std::unique_ptr<DomainState> state);
    const AbstractDomain& implementation() const
    {
        return *domain_;
    }
    void requireCompatible(const AbstractState& other) const;

    std::shared_ptr<const AbstractDomain> domain_;
    Environment environment_;
    std::unique_ptr<DomainState> state_;
};

} // namespace relational

#endif // RELATIONAL_ABSTRACT_DOMAIN_H
