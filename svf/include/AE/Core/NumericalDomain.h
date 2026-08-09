//===- NumericalDomain.h -- Shared numerical-domain API --------*- C++ -*-===//

#ifndef SVF_AE_NUMERICAL_DOMAIN_H
#define SVF_AE_NUMERICAL_DOMAIN_H

#include "AE/Core/AbstractState.h"
#include "AE/Core/LinearConstraint.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace SVF::AbstractDomain
{

enum class ApproximationKind
{
    Exact,
    SoundOverApproximation,
    UnsupportedFallback
};

enum class OperationKind
{
    Assignment,
    Assumption
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
    /// True only when nonlinear TreeExpression operations retain domain facts
    /// instead of applying a sound forget/ignore fallback.
    bool nonlinearTreeExpressions = false;
};

struct IntervalBox
{
    std::map<Variable, Interval> bounds;
};

struct WideningPolicy
{
    /// Constants used to delay a bound's jump to infinity.
    std::vector<Rational> thresholds;
};

/// Common interface for numerical abstract states. The representation and
/// lattice algorithms remain domain-specific; clients such as the SVF adapter
/// and test oracles only need this transfer/query surface.
class NumericalState : public AbstractState
{
public:
    ~NumericalState() override = default;

    virtual DomainCapabilities capabilities() const = 0;
    virtual const VariableEnvironment& environment() const = 0;

    virtual void assign(Variable target,
                        const LinearExpression& expression) = 0;
    virtual void assign(Variable target, const TreeExpression& expression) = 0;
    virtual void assume(const LinearConstraint& constraint) = 0;
    virtual void assume(const TreeConstraint& constraint) = 0;
    virtual void forget(Variable variable) = 0;
    virtual void changeEnvironment(
        const VariableEnvironment& environment,
        bool initializeNewVariablesToZero = false) = 0;

    virtual CheckResult entails(const LinearConstraint& constraint) const = 0;
    virtual Interval bound(Variable variable) const = 0;
    virtual IntervalBox toBox() const = 0;
    virtual LinearConstraintSet toConstraints() const = 0;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_NUMERICAL_DOMAIN_H
