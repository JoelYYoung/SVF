//===- AbstractDomain.h -- Abstract relational-state API -------*- C++ -*-===//

#ifndef RELATIONAL_ABSTRACT_DOMAIN_H
#define RELATIONAL_ABSTRACT_DOMAIN_H

#include "AE/Core/LinearConstraint.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace relational
{

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

/// One element of a numerical abstract domain.
///
/// Concrete states own both their representation and their domain-specific
/// algorithms.  This base supplies the common tree-expression fallback,
/// lattice plumbing, compatibility checks, entailment, and exports.  The file
/// retains its historical name because SVF already has AE/Core/AbstractState.h
/// for the full program state.
class AbstractState
{
public:
    virtual ~AbstractState();

    virtual std::unique_ptr<AbstractState> clone() const = 0;
    virtual const char* name() const = 0;
    virtual DomainCapabilities capabilities() const = 0;

    const Environment& environment() const
    {
        return environment_;
    }

    void assign(Variable target, const LinearExpression& expression);
    void assign(Variable target, const TreeExpression& expression);
    void assume(const LinearConstraint& constraint);
    void assume(const TreeConstraint& constraint);
    void forget(Variable variable);

    std::unique_ptr<AbstractState> joined(const AbstractState& other) const;
    std::unique_ptr<AbstractState> met(const AbstractState& other) const;
    std::unique_ptr<AbstractState> widened(
        const AbstractState& next,
        const WideningPolicy& policy = {}) const;
    std::unique_ptr<AbstractState> narrowed(const AbstractState& next) const;
    std::unique_ptr<AbstractState> projectLowerBounds() const;
    std::unique_ptr<AbstractState> changedEnvironment(
        const Environment& environment,
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

protected:
    explicit AbstractState(Environment environment);
    AbstractState(const AbstractState&) = default;
    AbstractState(AbstractState&&) noexcept = default;
    AbstractState& operator=(const AbstractState&) = default;
    AbstractState& operator=(AbstractState&&) noexcept = default;

    void requireCompatible(const AbstractState& other) const;
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason) const;

private:
    virtual DiagnosticSink* diagnosticSink() const = 0;
    virtual bool hasCompatibleDomain(const AbstractState& other) const = 0;

    virtual ApproximationKind assignState(
        Variable target, const LinearExpression& expression) = 0;
    virtual ApproximationKind assumeState(
        const LinearConstraint& constraint) = 0;
    virtual void forgetState(Variable variable) = 0;

    virtual void joinState(const AbstractState& other) = 0;
    virtual void meetState(const AbstractState& other) = 0;
    virtual void widenState(const AbstractState& next,
                            const WideningPolicy& policy) = 0;
    virtual void narrowState(const AbstractState& next) = 0;
    virtual void projectLowerBoundsState() = 0;
    virtual void changeEnvironmentState(
        const Environment& oldEnvironment,
        const Environment& newEnvironment,
        bool projectNewVariables) = 0;

    virtual bool isBottomState() const = 0;
    virtual bool isTopState() const = 0;
    virtual bool leqState(const AbstractState& other) const = 0;
    virtual Interval boundState(Variable variable) const = 0;
    virtual LinearConstraintSet constraintsState() const = 0;
    virtual std::string stateToString() const = 0;

    Environment environment_;
};

} // namespace relational

#endif // RELATIONAL_ABSTRACT_DOMAIN_H
