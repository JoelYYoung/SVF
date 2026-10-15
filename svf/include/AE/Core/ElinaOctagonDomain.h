//===- ElinaOctagonDomain.h -- ELINA Octagon adapter ---------*- C++ -*-===//

#ifndef SVF_AE_ELINA_OCTAGON_DOMAIN_H
#define SVF_AE_ELINA_OCTAGON_DOMAIN_H

#include "AE/Core/Expression.h"
#include "AE/Core/NumericalDomain.h"
#include "AE/Core/OctagonDomain.h"

#include <memory>
#include <vector>

namespace SVF::AbstractDomain
{

struct ElinaOctagonCapabilities
{
    bool exactLinearAssignment = true;
    bool exactLinearAssumption = true;
    bool parallelAssignment = true;
    bool substitution = true;
    bool projection = true;
    bool expandFold = true;
    bool widening = true;
    bool topologicalClosure = true;
    bool canonicalization = false;
    bool minimization = false;
    bool narrowing = false;
    bool thresholdWidening = false;
};

/// ELINA f524156d Octagon adapter. The backend is built only when CMake finds
/// the explicitly requested fixed ELINA installation. It saves and restores
/// the caller's FPU mode around every ELINA operation.
class ElinaOctagonDomain final : public NumericalDomain
{
public:
    class Impl;

    using NumericalDomain::assignParallel;
    using NumericalDomain::bound;
    using NumericalDomain::substitute;
    using NumericalDomain::substituteParallel;

    static ElinaOctagonDomain top(const OctagonConfig& config = {});
    static ElinaOctagonDomain bottom(const OctagonConfig& config = {});
    static ElinaOctagonDomain fromConstraints(
        const LinearConstraintSet& constraints,
        const OctagonConfig& config = {});
    static ElinaOctagonCapabilities capabilities() noexcept;
    static bool runtimeFpuSupported() noexcept;

    ElinaOctagonDomain(const ElinaOctagonDomain& other);
    ElinaOctagonDomain(ElinaOctagonDomain&& other) noexcept;
    ElinaOctagonDomain& operator=(const ElinaOctagonDomain& other);
    ElinaOctagonDomain& operator=(ElinaOctagonDomain&& other) noexcept;
    ~ElinaOctagonDomain() override;

    DomainKind kind() const noexcept override
    {
        return DomainKind::Octagon;
    }
    std::unique_ptr<AbstractDomain> clone() const override;
    const char* name() const noexcept;
    const OctagonConfig& config() const noexcept;

    void assign(Variable target,
                const LinearExpression& expression) override;
    void assign(Variable target, const TreeExpression& expression) override;
    void assignParallel(const LinearAssignmentList& assignments) override;
    void substitute(Variable target,
                    const LinearExpression& expression) override;
    void substituteParallel(
        const LinearAssignmentList& assignments) override;
    void assume(const LinearConstraint& constraint) override;
    void assumeAll(const LinearConstraintSet& constraints) override;
    void assume(const TreeConstraint& constraint) override;
    void forget(Variable variable) override;
    void project(const std::vector<Variable>& retained) override;
    void expand(Variable source,
                const std::vector<Variable>& copies) override;
    void fold(Variable target,
              const std::vector<Variable>& folded) override;

    CheckResult entails(const LinearConstraint& constraint) const override;
    /// Tri-state inclusion that preserves ELINA's false-without-exact result.
    CheckResult subsetOf(const ElinaOctagonDomain& other) const;
    Interval bound(Variable variable) const override;
    Interval bound(const LinearExpression& expression) const override;
    std::vector<Variable> supportVariables() const override;
    LinearConstraintSet toConstraints() const override;
    void close() override;
    void canonicalize() override;

    ElinaOctagonDomain join(const ElinaOctagonDomain& other) const;
    ElinaOctagonDomain meet(const ElinaOctagonDomain& other) const;
    ElinaOctagonDomain widen(
        const ElinaOctagonDomain& next,
        const WideningPolicy& policy = {}) const;
    ElinaOctagonDomain narrow(const ElinaOctagonDomain& next) const;

private:
    explicit ElinaOctagonDomain(OctagonConfig config, bool bottom);
    ElinaOctagonDomain(OctagonConfig config, std::unique_ptr<Impl> impl);

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<ElinaOctagonDomain>();
    }
    bool hasCompatibleDomain(const AbstractDomain& other) const override;
    void joinDomain(const AbstractDomain& other) override;
    void meetDomain(const AbstractDomain& other) override;
    void widenDomain(const AbstractDomain& next) override;
    void narrowDomain(const AbstractDomain& next) override;
    bool isBottomDomain() const override;
    bool isTopDomain() const override;
    bool leqDomain(const AbstractDomain& other) const override;
    std::string domainToString() const override;
    void assignInterval(Variable target, const Interval& value) override;
    const ElinaOctagonDomain& requireElina(
        const AbstractDomain& other) const;
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason = {}, bool best = true) const;

    OctagonConfig config_;
    std::unique_ptr<Impl> impl_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_ELINA_OCTAGON_DOMAIN_H
