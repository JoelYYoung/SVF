//===- OctagonDomain.h -- Exact-rational Octagon state ---------*- C++ -*-===//

#ifndef RELATIONAL_OCTAGON_DOMAIN_H
#define RELATIONAL_OCTAGON_DOMAIN_H

#include "AE/Core/AbstractState.h"
#include "AE/Core/LinearConstraint.h"

#include <map>
#include <memory>
#include <vector>

namespace SVF
{

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

struct WideningPolicy
{
    /// Constants for normalized +/-x +/-y <= c and +/-x <= c templates.
    std::vector<Rational> thresholds;
};

struct OctagonConfig
{
    bool strongClosure = true;
    bool integerTightening = true;
    std::shared_ptr<DiagnosticSink> diagnostics;

    /// Diagnostics affect observation only, not abstract-state semantics.
    bool operationCompatible(const OctagonConfig& other) const
    {
        return strongClosure == other.strongClosure &&
               integerTightening == other.integerTightening;
    }
};

/// One Octagon abstract element for constraints +/-x +/-y <= c.
///
/// Unlike APRON's manager/value split, this object owns its environment,
/// configuration, DBM representation, and algorithms.  Copies are deep value
/// copies; clone() supplies polymorphic ownership when a caller needs the
/// AbstractState interface.
class OctagonState final : public AbstractState
{
public:
    static OctagonState top(const Environment& environment,
                            const OctagonConfig& config = {});
    static OctagonState bottom(const Environment& environment,
                               const OctagonConfig& config = {});
    static OctagonState fromBox(const Environment& environment,
                                const Box& box,
                                const OctagonConfig& config = {});
    static OctagonState fromConstraints(
        const Environment& environment,
        const LinearConstraintSet& constraints,
        const OctagonConfig& config = {});

    OctagonState(const OctagonState& other);
    OctagonState(OctagonState&& other) noexcept;
    OctagonState& operator=(const OctagonState& other);
    OctagonState& operator=(OctagonState&& other) noexcept;
    ~OctagonState() override;

    std::unique_ptr<AbstractState> clone() const override;
    const char* name() const override;
    DomainCapabilities capabilities() const;

    const Environment& environment() const
    {
        return environment_;
    }

    void assign(Variable target, const LinearExpression& expression);
    void assign(Variable target, const TreeExpression& expression);
    void assume(const LinearConstraint& constraint);
    void assume(const TreeConstraint& constraint);
    void forget(Variable variable);
    void projectLowerBounds();
    void changeEnvironment(const Environment& environment,
                           bool projectNewVariables = false);

    CheckResult entails(const LinearConstraint& constraint) const;
    Interval bound(Variable variable) const;
    Box toBox() const;
    LinearConstraintSet toConstraints() const;

    const OctagonConfig& config() const;

    /// Explicitly converts the operation policy while retaining the represented
    /// concrete set. Enabling stronger normalization may improve precision;
    /// disabling it keeps existing facts but changes subsequent scheduling.
    OctagonState reconfigured(const OctagonConfig& config) const;

    OctagonState joinedOctagon(const OctagonState& other) const;
    OctagonState metOctagon(const OctagonState& other) const;
    OctagonState widenedOctagon(
        const OctagonState& next,
        const WideningPolicy& policy = {}) const;
    OctagonState narrowedOctagon(const OctagonState& next) const;
    OctagonState lowerBoundsProjection() const;
    OctagonState changedEnvironmentOctagon(
        const Environment& environment,
        bool projectNewVariables = false) const;

private:
    class Impl;

    OctagonState(Environment environment, OctagonConfig config, bool bottom);
    OctagonState(Environment environment, std::unique_ptr<Impl> impl);

    DiagnosticSink* diagnosticSink() const;
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason) const;
    bool hasCompatibleDomain(const AbstractState& other) const override;
    ApproximationKind assignState(
        Variable target, const LinearExpression& expression);
    ApproximationKind assumeState(
        const LinearConstraint& constraint);
    void forgetState(Variable variable);
    void joinState(const AbstractState& other) override;
    void meetState(const AbstractState& other) override;
    void widenState(const AbstractState& next) override;
    void narrowState(const AbstractState& next) override;
    void projectLowerBoundsState();
    void changeEnvironmentState(const Environment& oldEnvironment,
                                const Environment& newEnvironment,
                                bool projectNewVariables);
    bool isBottomState() const override;
    bool isTopState() const override;
    bool leqState(const AbstractState& other) const override;
    Interval boundState(Variable variable) const;
    LinearConstraintSet constraintsState() const;
    std::string stateToString() const override;

    const OctagonState& requireOctagon(const AbstractState& other) const;

    Environment environment_;
    std::unique_ptr<Impl> impl_;
};

} // namespace SVF

#endif // RELATIONAL_OCTAGON_DOMAIN_H
