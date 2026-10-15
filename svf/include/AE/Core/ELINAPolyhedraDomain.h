//===- ELINAPolyhedraDomain.h -- ELINA Polyhedra adapter -*- C++ -*-===//

#ifndef SVF_AE_ELINA_POLYHEDRA_DOMAIN_H
#define SVF_AE_ELINA_POLYHEDRA_DOMAIN_H

#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/ELINABackendContract.h"

#include <memory>

namespace SVF::AbstractDomain
{

struct ELINAPolyhedraConfig
{
    std::shared_ptr<DiagnosticSink> diagnostics;
    bool integerTightening = true;

    bool operationCompatible(const ELINAPolyhedraConfig& other) const
    {
        return integerTightening == other.integerTightening;
    }
};

/// Fixed-f524156d ELINA Polyhedra adapter.
///
/// Common operations execute directly on the ELINA value, which is the
/// authoritative observable state. The native implementation is materialized
/// only for an explicitly diagnosed unsupported/exception fallback.
class ELINAPolyhedraDomain final : public NumericalDomain
{
public:
    using NumericalDomain::assignParallel;
    using NumericalDomain::bound;
    using NumericalDomain::substitute;
    using NumericalDomain::substituteParallel;

    static ELINAPolyhedraDomain top(const ELINAPolyhedraConfig& config = {});
    static ELINAPolyhedraDomain bottom(const ELINAPolyhedraConfig& config = {});

    ELINAPolyhedraDomain(const ELINAPolyhedraDomain& other);
    ELINAPolyhedraDomain(ELINAPolyhedraDomain&& other) noexcept;
    ELINAPolyhedraDomain& operator=(const ELINAPolyhedraDomain& other);
    ELINAPolyhedraDomain& operator=(ELINAPolyhedraDomain&& other) noexcept;
    ~ELINAPolyhedraDomain() override;

    DomainKind kind() const noexcept override
    {
        return DomainKind::ConvexPolyhedra;
    }
    std::unique_ptr<AbstractDomain> clone() const override;

    void assign(Variable target, const LinearExpression& expression) override;
    void assign(Variable target, const TreeExpression& expression) override;
    void assignParallel(const LinearAssignmentList& assignments) override;
    void substitute(Variable target,
                    const LinearExpression& expression) override;
    void substituteParallel(const LinearAssignmentList& assignments) override;
    void assume(const LinearConstraint& constraint) override;
    void assumeAll(const LinearConstraintSet& constraints) override;
    void assume(const TreeConstraint& constraint) override;
    void forget(Variable variable) override;
    void project(const std::vector<Variable>& retained) override;
    void expand(Variable source, const std::vector<Variable>& copies) override;
    void fold(Variable target, const std::vector<Variable>& folded) override;

    CheckResult entails(const LinearConstraint& constraint) const override;
    Interval bound(Variable variable) const override;
    Interval bound(const LinearExpression& expression) const override;
    std::vector<Variable> supportVariables() const override;
    LinearConstraintSet toConstraints() const override;
    void close() override;
    void canonicalize() override;

    /// Directly expose ELINA's tri-state inclusion contract. AbstractDomain's
    /// historical bool lattice hook cannot represent Unknown, so lattice
    /// dispatch conservatively uses the checked native fallback.
    CheckResult backendInclusion(const ELINAPolyhedraDomain& other) const;

    const ELINAPolyhedraConfig& config() const
    {
        return config_;
    }

private:
    class Impl;
    ELINAPolyhedraDomain(ELINAPolyhedraConfig config, bool bottom);
    ConvexPolyhedraDomain nativeSnapshot() const;
    void replaceFromNative(const ConvexPolyhedraDomain& native,
                           OperationKind operation,
                           const std::string& fallbackOperation);
    void ensureVariables(const std::vector<Variable>& variables);
    void removeVariables(const std::vector<Variable>& variables);
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason, bool best = true) const;

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<ELINAPolyhedraDomain>();
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

    const ELINAPolyhedraDomain& requireELINA(const AbstractDomain& other) const;

    ELINAPolyhedraConfig config_;
    std::unique_ptr<Impl> impl_;
    /// Active only after an operation reaches syntax or numeric sorts that
    /// fixed f524156d's optimized Polyhedra carrier cannot represent. In
    /// particular, opt_pk_dimension reports every coordinate as integer and
    /// hard-codes realdim to zero.
    std::unique_ptr<ConvexPolyhedraDomain> fallback_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_ELINA_POLYHEDRA_DOMAIN_H
