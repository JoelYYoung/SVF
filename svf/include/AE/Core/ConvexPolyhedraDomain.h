//===- ConvexPolyhedraDomain.h -- Exact rational polyhedra ----*- C++ -*-===//

#ifndef SVF_AE_CONVEX_POLYHEDRA_DOMAIN_H
#define SVF_AE_CONVEX_POLYHEDRA_DOMAIN_H

#include "AE/Core/AbstractState.h"
#include "AE/Core/NumericalDomain.h"

#include <memory>
#include <vector>

namespace SVF::AbstractDomain
{

struct ConvexPolyhedraConfig
{
    std::shared_ptr<DiagnosticSink> diagnostics;

    bool operationCompatible(const ConvexPolyhedraConfig&) const
    {
        return true;
    }
};

/// Closed/non-closed convex polyhedra over exact GMP rationals. The internal
/// H-representation is normalized linear inequalities; projection, affine
/// image, feasibility, and convex hull use exact Fourier-Motzkin elimination.
class ConvexPolyhedraState final : public NumericalState
{
public:
    static ConvexPolyhedraState top(
        const VariableEnvironment& environment,
        const ConvexPolyhedraConfig& config = {});
    static ConvexPolyhedraState bottom(
        const VariableEnvironment& environment,
        const ConvexPolyhedraConfig& config = {});
    static ConvexPolyhedraState fromBox(
        const VariableEnvironment& environment, const IntervalBox& box,
        const ConvexPolyhedraConfig& config = {});
    static ConvexPolyhedraState fromConstraints(
        const VariableEnvironment& environment,
        const LinearConstraintSet& constraints,
        const ConvexPolyhedraConfig& config = {});

    ConvexPolyhedraState(const ConvexPolyhedraState& other);
    ConvexPolyhedraState(ConvexPolyhedraState&& other) noexcept;
    ConvexPolyhedraState& operator=(const ConvexPolyhedraState& other);
    ConvexPolyhedraState& operator=(ConvexPolyhedraState&& other) noexcept;
    ~ConvexPolyhedraState() override;

    std::unique_ptr<AbstractState> clone() const override;
    const char* name() const override;
    DomainCapabilities capabilities() const override;

    const VariableEnvironment& environment() const override
    {
        return environment_;
    }
    const ConvexPolyhedraConfig& config() const
    {
        return config_;
    }

    void assign(Variable target,
                const LinearExpression& expression) override;
    void assign(Variable target, const TreeExpression& expression) override;
    void assume(const LinearConstraint& constraint) override;
    void assume(const TreeConstraint& constraint) override;
    void forget(Variable variable) override;
    void changeEnvironment(const VariableEnvironment& environment,
                           bool initializeNewVariablesToZero = false) override;

    CheckResult entails(const LinearConstraint& constraint) const override;
    Interval bound(Variable variable) const override;
    IntervalBox toBox() const override;
    LinearConstraintSet toConstraints() const override;

    ConvexPolyhedraState join(
        const ConvexPolyhedraState& other) const;
    ConvexPolyhedraState meet(
        const ConvexPolyhedraState& other) const;
    ConvexPolyhedraState widen(
        const ConvexPolyhedraState& next) const;
    ConvexPolyhedraState narrow(
        const ConvexPolyhedraState& next) const;

private:
    class Impl;
    ConvexPolyhedraState(VariableEnvironment environment,
                         ConvexPolyhedraConfig config, bool bottom);

    bool hasCompatibleDomain(const AbstractState& other) const override;
    void joinState(const AbstractState& other) override;
    void meetState(const AbstractState& other) override;
    void widenState(const AbstractState& next) override;
    void narrowState(const AbstractState& next) override;
    bool isBottomState() const override;
    bool isTopState() const override;
    bool leqState(const AbstractState& other) const override;
    std::string stateToString() const override;

    const ConvexPolyhedraState& requirePolyhedron(
        const AbstractState& other) const;
    void addConstraint(const LinearConstraint& constraint);
    void normalize();
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason) const;

    VariableEnvironment environment_;
    ConvexPolyhedraConfig config_;
    std::unique_ptr<Impl> impl_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_CONVEX_POLYHEDRA_DOMAIN_H
