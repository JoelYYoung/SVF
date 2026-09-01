//===- OctagonDomain.h -- Exact-rational Octagon state ---------*- C++ -*-===//

#ifndef SVF_AE_OCTAGON_DOMAIN_H
#define SVF_AE_OCTAGON_DOMAIN_H

#include "AE/Core/AbstractState.h"
#include "AE/Core/NumericalDomain.h"

#include <memory>
#include <string>

namespace SVF::AbstractDomain
{

/// Selects the physical carrier of an Octagon DBM.  This is deliberately not
/// part of the abstract semantics: states using different carriers may be
/// combined, compared, and converted without approximation.
enum class OctagonStorageKind
{
    DenseHalf,
    SparseFinite,
    ComponentDense
};

const char* octagonStorageKindName(OctagonStorageKind kind);
OctagonStorageKind octagonStorageKindFromName(const std::string& name);

struct OctagonStorageStats
{
    OctagonStorageKind kind = OctagonStorageKind::DenseHalf;
    std::size_t dimensions = 0;
    std::size_t finiteStoredSlots = 0;
    std::size_t allocatedBoundSlots = 0;
    std::size_t components = 0;
    std::size_t maximumComponent = 0;
};

struct OctagonConfig
{
    bool strongClosure = true;
    bool integerTightening = true;
    OctagonStorageKind storage = OctagonStorageKind::DenseHalf;
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
/// configuration, DBM representation, and algorithms. Copies have value
/// semantics; component matrices may be shared copy-on-write until mutation.
/// clone() supplies polymorphic ownership when a caller needs the
/// AbstractState interface.
class OctagonState final : public NumericalState
{
public:
    using NumericalState::bound;
    using NumericalState::substitute;
    using NumericalState::substituteParallel;

    static OctagonState top(const VariableEnvironment& environment,
                            const OctagonConfig& config = {});
    static OctagonState bottom(const VariableEnvironment& environment,
                               const OctagonConfig& config = {});
    static OctagonState fromBox(const VariableEnvironment& environment,
                                const IntervalBox& box,
                                const OctagonConfig& config = {});
    static OctagonState fromConstraints(
        const VariableEnvironment& environment,
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

    const VariableEnvironment& environment() const override
    {
        return environment_;
    }

    void assign(Variable target,
                const LinearExpression& expression) override;
    void assign(Variable target, const TreeExpression& expression) override;
    void substitute(Variable target,
                    const LinearExpression& expression) override;
    void substituteParallel(
        const LinearAssignmentList& assignments) override;
    void assume(const LinearConstraint& constraint) override;
    void assume(const TreeConstraint& constraint) override;
    void assumeAll(const LinearConstraintSet& constraints) override;
    void forget(Variable variable) override;
    void assignInterval(Variable target, const Interval& value) override;
    void projectLowerBounds();
    void changeEnvironment(const VariableEnvironment& environment,
                           bool initializeNewVariablesToZero = false) override;
    void expand(Variable source,
                const std::vector<VariableDeclaration>& copies) override;
    void fold(Variable target,
              const std::vector<Variable>& folded) override;

    CheckResult entails(const LinearConstraint& constraint) const override;
    Interval bound(Variable variable) const override;
    Interval bound(const LinearExpression& expression) const override;
    IntervalBox toBox() const override;
    LinearConstraintSet toConstraints() const override;
    void close() override;
    void canonicalize() override;

    const OctagonConfig& config() const;
    OctagonStorageStats storageStats() const;

    /// Explicitly converts the operation policy while retaining the represented
    /// concrete set. Enabling stronger normalization may improve precision;
    /// disabling it keeps existing facts but changes subsequent scheduling.
    OctagonState reconfigured(const OctagonConfig& config) const;

    OctagonState join(const OctagonState& other) const;
    OctagonState meet(const OctagonState& other) const;
    OctagonState widen(
        const OctagonState& next,
        const WideningPolicy& policy = {}) const;
    OctagonState narrow(const OctagonState& next) const;
    OctagonState projectedLowerBounds() const;
    OctagonState withEnvironment(
        const VariableEnvironment& environment,
        bool initializeNewVariablesToZero = false) const;

private:
    class Impl;

    OctagonState(VariableEnvironment environment, OctagonConfig config, bool bottom);
    OctagonState(VariableEnvironment environment, std::unique_ptr<Impl> impl);

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<OctagonState>();
    }
    DiagnosticSink* diagnosticSink() const;
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason, bool best = true) const;
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
    void changeEnvironmentState(const VariableEnvironment& oldEnvironment,
                                const VariableEnvironment& newEnvironment,
                                bool initializeNewVariablesToZero);
    bool isBottomState() const override;
    bool isTopState() const override;
    bool leqState(const AbstractState& other) const override;
    Interval boundState(Variable variable) const;
    LinearConstraintSet constraintsState() const;
    std::string stateToString() const override;

    const OctagonState& requireOctagon(const AbstractState& other) const;

    VariableEnvironment environment_;
    std::unique_ptr<Impl> impl_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_OCTAGON_DOMAIN_H
