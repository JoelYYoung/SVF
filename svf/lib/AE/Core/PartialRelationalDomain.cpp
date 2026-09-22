//===- PartialRelationalDomain.cpp -- Bounded relational product ---------===//

#include "AE/Core/PartialRelationalDomain.h"

#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/Expression.h"
#include "AE/Core/OctagonDomain.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace SVF::AbstractDomain
{

namespace
{

std::unique_ptr<NumericalDomain> makeRelational(DomainKind kind, bool bottom)
{
    if (kind == DomainKind::Octagon)
    {
        OctagonConfig config;
        config.storage = OctagonStorageKind::ComponentDense;
        return std::make_unique<OctagonDomain>(
            bottom ? OctagonDomain::bottom(config)
                   : OctagonDomain::top(config));
    }
    if (kind == DomainKind::ConvexPolyhedra)
        return std::make_unique<ConvexPolyhedraDomain>(
            bottom ? ConvexPolyhedraDomain::bottom()
                   : ConvexPolyhedraDomain::top());
    throw std::invalid_argument(
        "partial relational domain requires Octagon or Convex Polyhedra");
}

std::unique_ptr<NumericalDomain> cloneNumerical(const NumericalDomain& domain)
{
    std::unique_ptr<AbstractDomain> clone = domain.clone();
    return std::unique_ptr<NumericalDomain>(
        static_cast<NumericalDomain*>(clone.release()));
}

LinearConstraint impossibleConstraint()
{
    return LinearConstraint(LinearExpression(Rational(1)),
                            ConstraintKind::LessEqual);
}

} // namespace

PartialRelationalDomain::PartialRelationalDomain(
    DomainKind relationalKind, std::shared_ptr<const Vocabulary> vocabulary,
    bool bottom)
    : relationalKind_(relationalKind), vocabulary_(std::move(vocabulary)),
      box_(bottom ? BoxDomain::bottom() : BoxDomain::top()),
      relational_(makeRelational(relationalKind, bottom))
{
    if (!vocabulary_)
        throw std::invalid_argument("partial relational vocabulary is null");
    if (!std::is_sorted(vocabulary_->begin(), vocabulary_->end()) ||
        std::adjacent_find(vocabulary_->begin(), vocabulary_->end()) !=
            vocabulary_->end())
        throw std::invalid_argument(
            "partial relational vocabulary must be sorted and unique");
    selected_.insert(vocabulary_->begin(), vocabulary_->end());
}

PartialRelationalDomain PartialRelationalDomain::top(
    DomainKind relationalKind, std::shared_ptr<const Vocabulary> vocabulary)
{
    return PartialRelationalDomain(relationalKind, std::move(vocabulary),
                                   false);
}

PartialRelationalDomain PartialRelationalDomain::bottom(
    DomainKind relationalKind, std::shared_ptr<const Vocabulary> vocabulary)
{
    return PartialRelationalDomain(relationalKind, std::move(vocabulary), true);
}

PartialRelationalDomain::PartialRelationalDomain(
    const PartialRelationalDomain& other)
    : NumericalDomain(other), relationalKind_(other.relationalKind_),
      vocabulary_(other.vocabulary_), selected_(other.selected_),
      box_(other.box_), relational_(cloneNumerical(*other.relational_))
{
}

PartialRelationalDomain& PartialRelationalDomain::operator=(
    const PartialRelationalDomain& other)
{
    if (this == &other)
        return *this;
    NumericalDomain::operator=(other);
    relationalKind_ = other.relationalKind_;
    vocabulary_ = other.vocabulary_;
    selected_ = other.selected_;
    box_ = other.box_;
    relational_ = cloneNumerical(*other.relational_);
    return *this;
}

std::unique_ptr<AbstractDomain> PartialRelationalDomain::clone() const
{
    return std::make_unique<PartialRelationalDomain>(*this);
}

bool PartialRelationalDomain::selected(Variable variable) const
{
    return selected_.count(variable) != 0;
}

bool PartialRelationalDomain::selected(const LinearExpression& expression) const
{
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)coefficient;
        if (!selected(variable))
            return false;
    }
    return true;
}

bool PartialRelationalDomain::selected(const TreeExpression& expression) const
{
    switch (expression.kind())
    {
    case TreeExpression::Kind::Constant:
        return true;
    case TreeExpression::Kind::Variable:
        return selected(expression.variable());
    case TreeExpression::Kind::Unary:
        return selected(expression.lhs());
    case TreeExpression::Kind::Binary:
        return selected(expression.lhs()) && selected(expression.rhs());
    }
    return false;
}

bool PartialRelationalDomain::selected(const LinearConstraint& constraint) const
{
    return selected(constraint.expression());
}

void PartialRelationalDomain::makeBottom()
{
    if (!box_.isBottom())
        box_.assume(impossibleConstraint());
    if (!relational_->isBottom())
        relational_->assume(impossibleConstraint());
}

void PartialRelationalDomain::importBoxBound(Variable variable)
{
    if (!selected(variable) || box_.isBottom())
        return;
    const Interval value = box_.bound(variable);
    if (value.isBottom())
    {
        makeBottom();
        return;
    }
    if (value.lower().isFinite())
        relational_->assume(LinearConstraint(
            LinearExpression(variable) -
                LinearExpression(value.lower().value()),
            value.lower().isStrict() ? ConstraintKind::GreaterThan
                                     : ConstraintKind::GreaterEqual));
    if (value.upper().isFinite())
        relational_->assume(LinearConstraint(
            LinearExpression(variable) -
                LinearExpression(value.upper().value()),
            value.upper().isStrict() ? ConstraintKind::LessThan
                                     : ConstraintKind::LessEqual));
}

void PartialRelationalDomain::synchronize()
{
    if (box_.isBottom() || relational_->isBottom())
    {
        makeBottom();
        return;
    }
    for (Variable variable : *vocabulary_)
        importBoxBound(variable);
    if (relational_->isBottom())
    {
        makeBottom();
        return;
    }
    for (Variable variable : relational_->supportVariables())
    {
        if (!selected(variable))
            throw std::logic_error(
                "partial relational backend escaped its fixed vocabulary");
        Interval reduced = box_.bound(variable);
        reduced.meetWith(relational_->bound(variable));
        if (reduced.isBottom())
        {
            makeBottom();
            return;
        }
        box_.assignBound(variable, reduced);
    }
}

void PartialRelationalDomain::assign(Variable target,
                                     const LinearExpression& expression)
{
    box_.assign(target, expression);
    const bool inside = selected(target) && selected(expression);
    if (inside)
        relational_->assign(target, expression);
    else if (selected(target))
    {
        relational_->forget(target);
        importBoxBound(target);
    }
    synchronize();
    recordPartialOperation(inside);
    recordOperation(
        OperationKind::Assignment,
        inside ? ApproximationKind::Exact
               : ApproximationKind::SoundOverApproximation,
        inside,
        inside ? std::string()
               : "assignment crossed the partial relational vocabulary");
}

void PartialRelationalDomain::assign(Variable target,
                                     const TreeExpression& expression)
{
    box_.assign(target, expression);
    const bool inside = selected(target) && selected(expression);
    if (inside)
        relational_->assign(target, expression);
    else if (selected(target))
    {
        relational_->forget(target);
        importBoxBound(target);
    }
    synchronize();
    recordPartialOperation(inside);
    recordOperation(
        OperationKind::Assignment,
        inside ? relational_->lastOperation().approximation
               : ApproximationKind::SoundOverApproximation,
        inside && relational_->lastOperation().best,
        inside ? relational_->lastOperation().reason
               : "tree assignment crossed the partial relational vocabulary");
}

void PartialRelationalDomain::assignParallel(
    const LinearAssignmentList& assignments)
{
    box_.assignParallel(assignments);
    bool allInside = true;
    LinearAssignmentList insideAssignments;
    std::vector<Variable> fallbackTargets;
    for (const LinearAssignment& assignment : assignments)
    {
        allInside &=
            selected(assignment.target) && selected(assignment.expression);
        if (!selected(assignment.target))
            continue;
        if (selected(assignment.expression))
            insideAssignments.push_back(assignment);
        else
            fallbackTargets.push_back(assignment.target);
    }
    relational_->assignParallel(insideAssignments);
    for (Variable target : fallbackTargets)
    {
        relational_->forget(target);
        importBoxBound(target);
    }
    synchronize();
    recordPartialOperation(allInside);
    recordOperation(
        OperationKind::Assignment,
        allInside ? ApproximationKind::Exact
                  : ApproximationKind::SoundOverApproximation,
        allInside,
        allInside
            ? std::string()
            : "parallel assignment crossed the partial relational vocabulary");
}

void PartialRelationalDomain::substitute(Variable target,
                                         const LinearExpression& expression)
{
    box_.substitute(target, expression);
    const bool inside = selected(target) && selected(expression);
    if (inside)
        relational_->substitute(target, expression);
    else if (selected(target))
        relational_->forget(target);
    synchronize();
    recordPartialOperation(inside);
    recordOperation(
        OperationKind::Substitution,
        inside ? ApproximationKind::Exact
               : ApproximationKind::SoundOverApproximation,
        inside,
        inside ? std::string()
               : "substitution crossed the partial relational vocabulary");
}

void PartialRelationalDomain::substituteParallel(
    const LinearAssignmentList& assignments)
{
    box_.substituteParallel(assignments);
    bool allInside = true;
    LinearAssignmentList insideAssignments;
    std::vector<Variable> fallbackTargets;
    for (const LinearAssignment& assignment : assignments)
    {
        allInside &=
            selected(assignment.target) && selected(assignment.expression);
        if (!selected(assignment.target))
            continue;
        if (selected(assignment.expression))
            insideAssignments.push_back(assignment);
        else
            fallbackTargets.push_back(assignment.target);
    }
    relational_->substituteParallel(insideAssignments);
    for (Variable target : fallbackTargets)
        relational_->forget(target);
    synchronize();
    recordPartialOperation(allInside);
    recordOperation(OperationKind::Substitution,
                    allInside ? ApproximationKind::Exact
                              : ApproximationKind::SoundOverApproximation,
                    allInside,
                    allInside ? std::string()
                              : "parallel substitution crossed the partial "
                                "relational vocabulary");
}

void PartialRelationalDomain::assume(const LinearConstraint& constraint)
{
    box_.assume(constraint);
    const bool inside = selected(constraint);
    if (inside)
        relational_->assume(constraint);
    synchronize();
    recordPartialOperation(inside);
    recordOperation(
        OperationKind::Assumption,
        inside ? relational_->lastOperation().approximation
               : ApproximationKind::SoundOverApproximation,
        inside && relational_->lastOperation().best,
        inside ? relational_->lastOperation().reason
               : "constraint crossed the partial relational vocabulary");
}

void PartialRelationalDomain::assume(const TreeConstraint& constraint)
{
    box_.assume(constraint);
    const bool inside = selected(constraint.expression());
    if (inside)
        relational_->assume(constraint);
    synchronize();
    recordPartialOperation(inside);
    recordOperation(
        OperationKind::Assumption,
        inside ? relational_->lastOperation().approximation
               : ApproximationKind::SoundOverApproximation,
        inside && relational_->lastOperation().best,
        inside ? relational_->lastOperation().reason
               : "tree constraint crossed the partial relational vocabulary");
}

void PartialRelationalDomain::assumeAll(const LinearConstraintSet& constraints)
{
    box_.assumeAll(constraints);
    LinearConstraintSet insideConstraints;
    for (const LinearConstraint& constraint : constraints)
        if (selected(constraint))
            insideConstraints.push_back(constraint);
    relational_->assumeAll(insideConstraints);
    synchronize();
    const bool inside = insideConstraints.size() == constraints.size();
    recordPartialOperation(inside);
    recordOperation(
        OperationKind::Assumption,
        inside ? ApproximationKind::Exact
               : ApproximationKind::SoundOverApproximation,
        inside,
        inside ? std::string()
               : "constraint batch crossed the partial relational vocabulary");
}

void PartialRelationalDomain::forget(Variable variable)
{
    box_.forget(variable);
    relational_->forget(variable);
    recordPartialProjection();
    recordOperation(OperationKind::Forget, ApproximationKind::Exact, true);
}

void PartialRelationalDomain::project(const std::vector<Variable>& retained)
{
    box_.project(retained);
    relational_->project(retained);
    synchronize();
    recordPartialProjection();
    recordOperation(OperationKind::Forget, ApproximationKind::Exact, true);
}

void PartialRelationalDomain::expand(Variable source,
                                     const std::vector<Variable>& copies)
{
    box_.expand(source, copies);
    bool inside = selected(source);
    for (Variable copy : copies)
        inside &= selected(copy);
    if (inside)
        relational_->expand(source, copies);
    else
    {
        for (Variable copy : copies)
            if (selected(copy))
            {
                relational_->forget(copy);
                importBoxBound(copy);
            }
    }
    synchronize();
    recordPartialOperation(inside);
    recordOperation(OperationKind::Expand,
                    inside ? ApproximationKind::Exact
                           : ApproximationKind::SoundOverApproximation,
                    inside,
                    inside
                        ? std::string()
                        : "expand crossed the partial relational vocabulary");
}

void PartialRelationalDomain::fold(Variable target,
                                   const std::vector<Variable>& folded)
{
    box_.fold(target, folded);
    bool inside = selected(target);
    for (Variable variable : folded)
        inside &= selected(variable);
    if (inside)
        relational_->fold(target, folded);
    else
    {
        if (selected(target))
        {
            relational_->forget(target);
            importBoxBound(target);
        }
        for (Variable variable : folded)
            relational_->forget(variable);
    }
    synchronize();
    recordPartialOperation(inside);
    recordOperation(OperationKind::Fold,
                    inside ? ApproximationKind::Exact
                           : ApproximationKind::SoundOverApproximation,
                    inside,
                    inside ? std::string()
                           : "fold crossed the partial relational vocabulary");
}

CheckResult PartialRelationalDomain::entails(
    const LinearConstraint& constraint) const
{
    if (selected(constraint))
    {
        const CheckResult result = relational_->entails(constraint);
        if (result == CheckResult::True)
            return result;
    }
    return box_.entails(constraint);
}

Interval PartialRelationalDomain::bound(Variable variable) const
{
    Interval result = box_.bound(variable);
    if (selected(variable))
        result.meetWith(relational_->bound(variable));
    return result;
}

Interval PartialRelationalDomain::bound(
    const LinearExpression& expression) const
{
    Interval result = box_.bound(expression);
    if (selected(expression))
        result.meetWith(relational_->bound(expression));
    return result;
}

std::vector<Variable> PartialRelationalDomain::supportVariables() const
{
    const std::vector<Variable> boxSupport = box_.supportVariables();
    const std::vector<Variable> relationalSupport =
        relational_->supportVariables();
    std::vector<Variable> result;
    result.reserve(boxSupport.size() + relationalSupport.size());
    std::set_union(boxSupport.begin(), boxSupport.end(),
                   relationalSupport.begin(), relationalSupport.end(),
                   std::back_inserter(result));
    return result;
}

std::vector<Variable> PartialRelationalDomain::relationalClosureState(
    const std::vector<Variable>& seeds) const
{
    // Box contributes only unary bounds, so every dependency edge in this
    // reduced product comes from the selected relational facet.
    return delegateRelationalClosureState(*relational_, seeds);
}

LinearConstraintSet PartialRelationalDomain::toConstraints() const
{
    LinearConstraintSet result = box_.toConstraints();
    LinearConstraintSet relationalConstraints = relational_->toConstraints();
    result.insert(result.end(), relationalConstraints.begin(),
                  relationalConstraints.end());
    return result;
}

void PartialRelationalDomain::close()
{
    box_.close();
    relational_->close();
    synchronize();
}

void PartialRelationalDomain::canonicalize()
{
    box_.canonicalize();
    relational_->canonicalize();
    synchronize();
}

const PartialRelationalDomain& PartialRelationalDomain::requirePartial(
    const AbstractDomain& other) const
{
    requireCompatible(other);
    return static_cast<const PartialRelationalDomain&>(other);
}

bool PartialRelationalDomain::hasCompatibleDomain(
    const AbstractDomain& other) const
{
    if (!other.isDomain<PartialRelationalDomain>())
        return false;
    const auto& partial = static_cast<const PartialRelationalDomain&>(other);
    return relationalKind_ == partial.relationalKind_ &&
           *vocabulary_ == *partial.vocabulary_ &&
           box_.isCompatibleWith(partial.box_) &&
           relational_->isCompatibleWith(*partial.relational_);
}

void PartialRelationalDomain::joinDomain(const AbstractDomain& other)
{
    const auto& partial = requirePartial(other);
    box_.joinWith(partial.box_);
    relational_->joinWith(*partial.relational_);
    synchronize();
    recordOperation(OperationKind::Join, ApproximationKind::Exact, true);
}

void PartialRelationalDomain::meetDomain(const AbstractDomain& other)
{
    const auto& partial = requirePartial(other);
    box_.meetWith(partial.box_);
    relational_->meetWith(*partial.relational_);
    synchronize();
    recordOperation(OperationKind::Meet, ApproximationKind::Exact, true);
}

void PartialRelationalDomain::widenDomain(const AbstractDomain& next)
{
    const auto& partial = requirePartial(next);
    box_.widenWith(partial.box_);
    relational_->widenWith(*partial.relational_);
    synchronize();
    recordOperation(OperationKind::Widening, ApproximationKind::Exact, true);
}

void PartialRelationalDomain::narrowDomain(const AbstractDomain& next)
{
    const auto& partial = requirePartial(next);
    box_.narrowWith(partial.box_);
    relational_->narrowWith(*partial.relational_);
    synchronize();
    recordOperation(OperationKind::Narrowing, ApproximationKind::Exact, true);
}

bool PartialRelationalDomain::isBottomDomain() const
{
    return box_.isBottom() || relational_->isBottom();
}

bool PartialRelationalDomain::isTopDomain() const
{
    return box_.isTop() && relational_->isTop();
}

bool PartialRelationalDomain::leqDomain(const AbstractDomain& other) const
{
    const auto& partial = requirePartial(other);
    return box_.isSubsetOf(partial.box_) == CheckResult::True &&
           relational_->isSubsetOf(*partial.relational_) == CheckResult::True;
}

std::string PartialRelationalDomain::domainToString() const
{
    std::ostringstream output;
    output << "PartialRelational(vocabulary=" << vocabulary_->size()
           << ", box=" << box_.toString()
           << ", relational=" << relational_->toString() << ')';
    return output.str();
}

} // namespace SVF::AbstractDomain
