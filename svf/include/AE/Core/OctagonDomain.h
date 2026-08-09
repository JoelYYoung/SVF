//===- OctagonDomain.h -- Exact-rational Octagon state ---------*- C++ -*-===//

#ifndef RELATIONAL_OCTAGON_DOMAIN_H
#define RELATIONAL_OCTAGON_DOMAIN_H

#include "AE/Core/AbstractDomain.h"

#include <memory>

namespace relational
{

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
    DomainCapabilities capabilities() const override;

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

    DiagnosticSink* diagnosticSink() const override;
    bool hasCompatibleDomain(const AbstractState& other) const override;
    ApproximationKind assignState(
        Variable target, const LinearExpression& expression) override;
    ApproximationKind assumeState(
        const LinearConstraint& constraint) override;
    void forgetState(Variable variable) override;
    void joinState(const AbstractState& other) override;
    void meetState(const AbstractState& other) override;
    void widenState(const AbstractState& next,
                    const WideningPolicy& policy) override;
    void narrowState(const AbstractState& next) override;
    void projectLowerBoundsState() override;
    void changeEnvironmentState(const Environment& oldEnvironment,
                                const Environment& newEnvironment,
                                bool projectNewVariables) override;
    bool isBottomState() const override;
    bool isTopState() const override;
    bool leqState(const AbstractState& other) const override;
    Interval boundState(Variable variable) const override;
    LinearConstraintSet constraintsState() const override;
    std::string stateToString() const override;

    const OctagonState& requireOctagon(const AbstractState& other) const;

    std::unique_ptr<Impl> impl_;
};

} // namespace relational

#endif // RELATIONAL_OCTAGON_DOMAIN_H
