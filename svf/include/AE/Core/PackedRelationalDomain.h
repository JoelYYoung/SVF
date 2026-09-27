// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_AE_PACKED_RELATIONAL_DOMAIN_H
#define SVF_AE_PACKED_RELATIONAL_DOMAIN_H
#include "AE/Core/OctagonDomain.h"
#include <functional>
#include <set>
namespace SVF::AbstractDomain
{
// Production NumericalDomain adapter. Fixed logical packs plus a global Box
// fallback for values absent from the static vocabulary (e.g. fresh objects).
// This does not select/change the fixpoint executor.
class PackedRelationalDomain final : public NumericalDomain
{
public:
    using Packing = std::vector<std::vector<Variable>>;
    using NumericalDomain::bound;
    using NumericalDomain::assignParallel;
    using NumericalDomain::substitute;
    using NumericalDomain::substituteParallel;
    explicit PackedRelationalDomain(std::shared_ptr<const Packing>, bool bottom = false);
    DomainKind kind() const noexcept override { return DomainKind::Octagon; }
    std::unique_ptr<AbstractDomain> clone() const override;
    void assign(Variable, const LinearExpression&) override;
    void assign(Variable, const TreeExpression&) override;
    void assignParallel(const LinearAssignmentList&) override;
    void substitute(Variable, const LinearExpression&) override;
    void substituteParallel(const LinearAssignmentList&) override;
    void assume(const LinearConstraint&) override;
    void assume(const TreeConstraint&) override;
    void assumeAll(const LinearConstraintSet&) override;
    void forget(Variable) override;
    void project(const std::vector<Variable>&) override;
    void expand(Variable, const std::vector<Variable>&) override;
    void fold(Variable, const std::vector<Variable>&) override;
    CheckResult entails(const LinearConstraint&) const override;
    Interval bound(Variable) const override;
    Interval bound(const LinearExpression&) const override;
    std::vector<Variable> supportVariables() const override;
    LinearConstraintSet toConstraints() const override;
    void close() override;
    void canonicalize() override;
    // Executor storage boundary: a pack is an indivisible abstract location.
    const OctagonDomain& packState(std::size_t index) const { return packs_.at(index); }
    const BoxDomain& fallbackState() const { return box_; }
    void restoreComponents(BoxDomain fallback, std::vector<OctagonDomain> packs);
private:
    const void* dynamicTypeToken() const noexcept override { return staticTypeToken<PackedRelationalDomain>(); }
    bool hasCompatibleDomain(const AbstractDomain&) const override;
    void joinDomain(const AbstractDomain&) override;
    void meetDomain(const AbstractDomain&) override;
    void widenDomain(const AbstractDomain&) override;
    void narrowDomain(const AbstractDomain&) override;
    bool isBottomDomain() const override;
    bool isTopDomain() const override;
    bool leqDomain(const AbstractDomain&) const override;
    std::string domainToString() const override;
    std::vector<Variable> relationalClosureState(const std::vector<Variable>&) const override;
    bool contains(std::size_t, Variable) const;
    void normalizeBottom();
    void importOutside(OctagonDomain&, std::size_t, const std::set<Variable>&) const;
    void update(const std::set<Variable>& targets, const std::set<Variable>& reads,
                const std::function<void(OctagonDomain&, std::size_t)>& operation);
    std::shared_ptr<const Packing> packing_;
    BoxDomain box_;
    std::vector<OctagonDomain> packs_;
};
}
#endif
