//===- PartialRelationalDomain.h -- Bounded relational product -*- C++ -*-===//
//
//                     SVF: Static Value-Flow Analysis
//
// This file is distributed under the same license as SVF.
//
//===----------------------------------------------------------------------===//

#ifndef SVF_AE_PARTIAL_RELATIONAL_DOMAIN_H
#define SVF_AE_PARTIAL_RELATIONAL_DOMAIN_H

#include "AE/Core/NumericalDomain.h"

#include <memory>
#include <set>
#include <vector>

namespace SVF::AbstractDomain
{

/// A reduced product that keeps sound Box bounds for every variable and a
/// stronger Octagon or Convex-Polyhedra property only for a fixed vocabulary.
/// The vocabulary is frozen before analysis so every state has a compatible
/// schema. Operations crossing its boundary retain their Box transformer and
/// conservatively replace affected relational coordinates by their Box hull.
class PartialRelationalDomain final : public NumericalDomain
{
public:
    using Vocabulary = std::vector<Variable>;
    using NumericalDomain::assignParallel;
    using NumericalDomain::bound;
    using NumericalDomain::substitute;
    using NumericalDomain::substituteParallel;

    static PartialRelationalDomain top(
        DomainKind relationalKind,
        std::shared_ptr<const Vocabulary> vocabulary);
    static PartialRelationalDomain bottom(
        DomainKind relationalKind,
        std::shared_ptr<const Vocabulary> vocabulary);

    PartialRelationalDomain(const PartialRelationalDomain& other);
    PartialRelationalDomain(PartialRelationalDomain&& other) noexcept = default;
    PartialRelationalDomain& operator=(const PartialRelationalDomain& other);
    PartialRelationalDomain& operator=(
        PartialRelationalDomain&& other) noexcept = default;
    ~PartialRelationalDomain() override = default;

    DomainKind kind() const noexcept override
    {
        return relationalKind_;
    }
    std::unique_ptr<AbstractDomain> clone() const override;

    void assign(Variable target, const LinearExpression& expression) override;
    void assign(Variable target, const TreeExpression& expression) override;
    void assignParallel(const LinearAssignmentList& assignments) override;
    void substitute(Variable target,
                    const LinearExpression& expression) override;
    void substituteParallel(const LinearAssignmentList& assignments) override;
    void assume(const LinearConstraint& constraint) override;
    void assume(const TreeConstraint& constraint) override;
    void assumeAll(const LinearConstraintSet& constraints) override;
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

    const Vocabulary& vocabulary() const
    {
        return *vocabulary_;
    }
    const BoxDomain& box() const
    {
        return box_;
    }
    const NumericalDomain& relational() const
    {
        return *relational_;
    }

private:
    PartialRelationalDomain(DomainKind relationalKind,
                            std::shared_ptr<const Vocabulary> vocabulary,
                            bool bottom);

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<PartialRelationalDomain>();
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
    std::vector<Variable> relationalClosureState(
        const std::vector<Variable>& seeds) const override;

    const PartialRelationalDomain& requirePartial(
        const AbstractDomain& other) const;
    bool selected(Variable variable) const;
    bool selected(const LinearExpression& expression) const;
    bool selected(const TreeExpression& expression) const;
    bool selected(const LinearConstraint& constraint) const;
    void synchronize();
    void importBoxBound(Variable variable);
    void makeBottom();

    DomainKind relationalKind_;
    std::shared_ptr<const Vocabulary> vocabulary_;
    std::set<Variable> selected_;
    BoxDomain box_;
    std::unique_ptr<NumericalDomain> relational_;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_PARTIAL_RELATIONAL_DOMAIN_H
