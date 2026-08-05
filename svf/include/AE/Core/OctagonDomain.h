//===- OctagonDomain.h -- Exact-rational Octagon domain --------*- C++ -*-===//

#ifndef RELATIONAL_OCTAGON_DOMAIN_H
#define RELATIONAL_OCTAGON_DOMAIN_H

#include "AE/Core/AbstractDomain.h"

#include <memory>

namespace relational
{

struct OctagonOptions
{
    bool strongClosure = true;
    bool integerTightening = true;
    std::shared_ptr<DiagnosticSink> diagnostics;
};

/// Concrete AbstractDomain for constraints of the form +/-x +/-y <= c.
class OctagonDomain final : public AbstractDomain
{
public:
    ~OctagonDomain() override;

    const char* name() const override;
    DomainCapabilities capabilities() const override;

private:
    friend std::shared_ptr<OctagonDomain> makeOctagonDomain(
        const OctagonOptions& options);

    explicit OctagonDomain(const OctagonOptions& options);

    std::unique_ptr<DomainState> makeTop(
        const Environment& environment) const override;
    std::unique_ptr<DomainState> makeBottom(
        const Environment& environment) const override;

    ApproximationKind assignState(
        DomainState& state, const Environment& environment, Variable target,
        const LinearExpression& expression) const override;
    ApproximationKind assumeState(
        DomainState& state, const Environment& environment,
        const LinearConstraint& constraint) const override;
    void forgetState(DomainState& state, const Environment& environment,
                     Variable variable) const override;

    std::unique_ptr<DomainState> joinStates(
        const DomainState& lhs, const DomainState& rhs) const override;
    std::unique_ptr<DomainState> meetStates(
        const DomainState& lhs, const DomainState& rhs) const override;
    std::unique_ptr<DomainState> widenStates(
        const DomainState& current, const DomainState& next,
        const WideningPolicy& policy) const override;
    std::unique_ptr<DomainState> narrowStates(
        const DomainState& current, const DomainState& next) const override;
    std::unique_ptr<DomainState> projectLowerBoundsState(
        const DomainState& state) const override;
    std::unique_ptr<DomainState> changeEnvironmentState(
        const DomainState& state, const Environment& oldEnvironment,
        const Environment& newEnvironment,
        bool projectNewVariables) const override;

    bool isBottomState(const DomainState& state) const override;
    bool isTopState(const DomainState& state) const override;
    bool leqStates(const DomainState& lhs,
                   const DomainState& rhs) const override;
    Interval boundState(const DomainState& state,
                        const Environment& environment,
                        Variable variable) const override;
    LinearConstraintSet constraintsState(
        const DomainState& state,
        const Environment& environment) const override;
    std::string stateToString(const DomainState& state,
                              const Environment& environment) const override;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

std::shared_ptr<OctagonDomain> makeOctagonDomain(
    const OctagonOptions& options = {});

} // namespace relational

#endif // RELATIONAL_OCTAGON_DOMAIN_H
