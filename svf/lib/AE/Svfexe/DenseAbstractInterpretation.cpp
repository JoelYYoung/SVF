//===- DenseAbstractInterpretation.cpp -- Domain-backed dense AE --------===//

#include "AE/Svfexe/DenseAbstractInterpretation.h"

#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <limits>
#include <stdexcept>
#include <utility>

using namespace SVF;
namespace AD = SVF::AbstractDomain;

namespace
{

s64_t toSigned64(const mpz_class& value, bool upper)
{
    if (!mpz_fits_slong_p(value.get_mpz_t()))
        return upper ? std::numeric_limits<s64_t>::max()
                     : std::numeric_limits<s64_t>::min();
    return static_cast<s64_t>(value.get_si());
}

BoundedInt lowerBound(const AD::Bound& bound)
{
    if (bound.isMinusInfinity())
        return IntervalValue::minus_infinity();
    if (bound.isPlusInfinity())
        return IntervalValue::plus_infinity();
    AD::Rational integer = bound.isStrict() ? bound.value().floor() +
                                                  AD::Rational(1)
                                            : bound.value().ceil();
    return BoundedInt(toSigned64(integer.value().get_num(), false));
}

BoundedInt upperBound(const AD::Bound& bound)
{
    if (bound.isPlusInfinity())
        return IntervalValue::plus_infinity();
    if (bound.isMinusInfinity())
        return IntervalValue::minus_infinity();
    AD::Rational integer = bound.isStrict() ? bound.value().ceil() -
                                                  AD::Rational(1)
                                            : bound.value().floor();
    return BoundedInt(toSigned64(integer.value().get_num(), true));
}

IntervalValue projectInterval(const AD::Interval& interval)
{
    if (interval.isBottom())
        return IntervalValue::bottom();
    return IntervalValue(lowerBound(interval.lower()),
                         upperBound(interval.upper()));
}

AD::ConstraintKind negatePredicate(AD::ConstraintKind kind)
{
    switch (kind)
    {
    case AD::ConstraintKind::Equal:
        return AD::ConstraintKind::NotEqual;
    case AD::ConstraintKind::NotEqual:
        return AD::ConstraintKind::Equal;
    case AD::ConstraintKind::LessThan:
        return AD::ConstraintKind::GreaterEqual;
    case AD::ConstraintKind::LessEqual:
        return AD::ConstraintKind::GreaterThan;
    case AD::ConstraintKind::GreaterThan:
        return AD::ConstraintKind::LessEqual;
    case AD::ConstraintKind::GreaterEqual:
        return AD::ConstraintKind::LessThan;
    }
    return kind;
}

bool constraintKind(s32_t predicate, AD::ConstraintKind& kind)
{
    switch (predicate)
    {
    case CmpStmt::ICMP_EQ:
        kind = AD::ConstraintKind::Equal;
        return true;
    case CmpStmt::ICMP_NE:
        kind = AD::ConstraintKind::NotEqual;
        return true;
    case CmpStmt::ICMP_SLT:
        kind = AD::ConstraintKind::LessThan;
        return true;
    case CmpStmt::ICMP_SLE:
        kind = AD::ConstraintKind::LessEqual;
        return true;
    case CmpStmt::ICMP_SGT:
        kind = AD::ConstraintKind::GreaterThan;
        return true;
    case CmpStmt::ICMP_SGE:
        kind = AD::ConstraintKind::GreaterEqual;
        return true;
    default:
        return false;
    }
}

} // namespace

template <typename NumericalStateT>
DenseAbstractInterpretation<NumericalStateT>::DenseAbstractInterpretation()
    : adapter_(*svfir)
{
}

template <typename NumericalStateT>
const AbstractDomain::AbstractState&
DenseAbstractInterpretation<NumericalStateT>::getAbstractState(
    const ICFGNode* node) const
{
    return state(node);
}

template <typename NumericalStateT>
typename DenseAbstractInterpretation<NumericalStateT>::DenseState
DenseAbstractInterpretation<NumericalStateT>::topState(
    const ICFGNode* node) const
{
    return DenseState(
        NumericalStateT::top(adapter_.environment(node ? node->getFun()
                                                        : nullptr)),
        adapter_.memoryLayout());
}

template <typename NumericalStateT>
typename DenseAbstractInterpretation<NumericalStateT>::DenseState
DenseAbstractInterpretation<NumericalStateT>::bottomState(
    const ICFGNode* node) const
{
    return DenseState(
        NumericalStateT::bottom(adapter_.environment(node ? node->getFun()
                                                           : nullptr)),
        adapter_.memoryLayout());
}

template <typename NumericalStateT>
typename DenseAbstractInterpretation<NumericalStateT>::DenseState&
DenseAbstractInterpretation<NumericalStateT>::ensureState(
    const ICFGNode* node)
{
    auto iterator = denseTrace_.find(node);
    if (iterator == denseTrace_.end())
        iterator = denseTrace_.emplace(node, topState(node)).first;
    abstractTrace.try_emplace(node);
    return iterator->second;
}

template <typename NumericalStateT>
const typename DenseAbstractInterpretation<NumericalStateT>::DenseState&
DenseAbstractInterpretation<NumericalStateT>::state(const ICFGNode* node) const
{
    const auto iterator = denseTrace_.find(node);
    if (iterator == denseTrace_.end())
        throw std::out_of_range("no dense abstract state for ICFG node");
    return iterator->second;
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::resetAbstractState(
    const ICFGNode* node)
{
    denseTrace_.insert_or_assign(node, topState(node));
    abstractTrace[node] = IntervalState();
    definedVariables_[node].clear();
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::copyAbstractState(
    const ICFGNode* source, const ICFGNode* destination)
{
    DenseState copy = state(source);
    copy.changeEnvironment(adapter_.environment(destination->getFun()));
    std::set<AD::Variable> copiedDefinitions = definedVariables_[source];
    for (auto iterator = copiedDefinitions.begin();
            iterator != copiedDefinitions.end();)
    {
        if (!copy.numerical().environment().contains(*iterator))
            iterator = copiedDefinitions.erase(iterator);
        else
            ++iterator;
    }
    denseTrace_.insert_or_assign(destination, std::move(copy));
    definedVariables_[destination] = std::move(copiedDefinitions);
    abstractTrace[destination] = abstractTrace[source];
    rebuildCompatibilityProjection(destination);
}

template <typename NumericalStateT>
std::unique_ptr<AbstractDomain::AbstractState>
DenseAbstractInterpretation<NumericalStateT>::cloneAbstractState(
    const ICFGNode* node) const
{
    return state(node).clone();
}

template <typename NumericalStateT>
bool DenseAbstractInterpretation<NumericalStateT>::isAbstractStateEquivalent(
    const ICFGNode* node,
    const AbstractDomain::AbstractState& snapshot) const
{
    return state(node).isEquivalentTo(snapshot) ==
           AbstractDomain::CheckResult::True;
}

template <typename NumericalStateT>
std::unique_ptr<AbstractDomain::AbstractState>
DenseAbstractInterpretation<NumericalStateT>::cloneCycleHeadState(
    const ICFGCycleWTO* cycle)
{
    return cloneAbstractState(cycle->head()->getICFGNode());
}

template <typename NumericalStateT>
bool DenseAbstractInterpretation<NumericalStateT>::widenCycleState(
    const AbstractDomain::AbstractState& previous,
    const AbstractDomain::AbstractState& current,
    const ICFGCycleWTO* cycle)
{
    const auto& previousDense = dynamic_cast<const DenseState&>(previous);
    const auto& currentDense = dynamic_cast<const DenseState&>(current);
    DenseState next = previousDense;
    next.widenWith(currentDense);
    const bool fixpoint =
        next.isEquivalentTo(previousDense) ==
        AbstractDomain::CheckResult::True;
    const ICFGNode* head = cycle->head()->getICFGNode();
    denseTrace_.insert_or_assign(head, std::move(next));
    rebuildCompatibilityProjection(head);
    return fixpoint;
}

template <typename NumericalStateT>
bool DenseAbstractInterpretation<NumericalStateT>::narrowCycleState(
    const AbstractDomain::AbstractState& previous,
    const AbstractDomain::AbstractState& current,
    const ICFGCycleWTO* cycle)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    if (!shouldApplyNarrowing(head->getFun()))
        return true;
    const auto& previousDense = dynamic_cast<const DenseState&>(previous);
    const auto& currentDense = dynamic_cast<const DenseState&>(current);
    DenseState next = previousDense;
    next.narrowWith(currentDense);
    const bool fixpoint =
        next.isEquivalentTo(previousDense) ==
        AbstractDomain::CheckResult::True;
    if (!fixpoint)
    {
        denseTrace_.insert_or_assign(head, std::move(next));
        rebuildCompatibilityProjection(head);
    }
    return fixpoint;
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::ensureVariable(
    DenseState& denseState, AD::Variable variable) const
{
    if (denseState.numerical().environment().contains(variable))
        return;
    denseState.changeEnvironment(
        denseState.numerical().environment().add(
            {adapter_.declaration(variable)}));
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::assignInterval(
    DenseState& denseState, AD::Variable variable,
    const IntervalValue& interval)
{
    ensureVariable(denseState, variable);
    denseState.numerical().forget(variable);
    constrainInterval(denseState, variable, interval);
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::constrainInterval(
    DenseState& denseState, AD::Variable variable,
    const IntervalValue& interval)
{
    if (interval.isBottom())
        return;

    AD::LinearExpression expression(variable);
    if (!interval.lb().is_minus_infinity())
    {
        denseState.assume(AD::greaterEqual(
            expression,
            AD::LinearExpression(
                AD::Rational(interval.lb().getIntNumeral()))));
    }
    if (!interval.ub().is_plus_infinity())
    {
        denseState.assume(AD::lessEqual(
            expression,
            AD::LinearExpression(
                AD::Rational(interval.ub().getIntNumeral()))));
    }
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::initializeDomainState(
    const ICFGNode* node)
{
    (void)ensureState(node);
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::assignDomainInterval(
    const ICFGNode* node, const SVFVar* target,
    const IntervalValue& interval)
{
    DenseState& denseState = ensureState(node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(target))
    {
        if (adapter_.contains(*value))
            assignInterval(denseState, adapter_.variable(*value), interval);
    }
    else if (const auto* object = SVFUtil::dyn_cast<ObjVar>(target))
    {
        if (adapter_.contains(*object))
            assignInterval(denseState, adapter_.contentVariable(*object),
                           interval);
    }
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::updateDomainOnBinary(
    const BinaryOPStmt* binary, const IntervalValue& result)
{
    DenseState& denseState = ensureState(binary->getICFGNode());
    const auto* target = SVFUtil::dyn_cast<ValVar>(binary->getRes());
    if (!target || !adapter_.contains(*target))
        return;
    const AD::Variable targetVariable = adapter_.variable(*target);

    auto operand = [&](const SVFVar* value,
                       AD::LinearExpression& expression) -> bool
    {
        if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(value))
        {
            expression = AD::LinearExpression(
                AD::Rational(integer->getSExtValue()));
            return true;
        }
        const auto* scalar = SVFUtil::dyn_cast<ValVar>(value);
        if (!scalar || !adapter_.contains(*scalar))
            return false;
        ensureVariable(denseState, adapter_.variable(*scalar));
        expression = AD::LinearExpression(adapter_.variable(*scalar));
        return true;
    };

    AD::LinearExpression lhs;
    AD::LinearExpression rhs;
    const bool affine =
        (binary->getOpcode() == BinaryOPStmt::Add ||
         binary->getOpcode() == BinaryOPStmt::Sub) &&
        operand(binary->getOpVar(0), lhs) &&
        operand(binary->getOpVar(1), rhs);
    if (!affine)
    {
        assignInterval(denseState, targetVariable, result);
        return;
    }

    ensureVariable(denseState, targetVariable);
    denseState.numerical().assign(
        targetVariable,
        binary->getOpcode() == BinaryOPStmt::Add ? lhs + rhs : lhs - rhs);
    constrainInterval(denseState, targetVariable, result);
    rebuildCompatibilityProjection(binary->getICFGNode());
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::updateDomainCopyValue(
    const ICFGNode* node, const SVFVar* target, const SVFVar* source,
    bool exactMathematicalCopy)
{
    const auto* targetValue = SVFUtil::dyn_cast<ValVar>(target);
    if (!targetValue || !adapter_.contains(*targetValue))
        return;
    DenseState& denseState = ensureState(node);
    const AD::Variable targetVariable = adapter_.variable(*targetValue);
    const AbstractValue& result = getAbsValue(targetValue, node);
    if (!result.isInterval())
    {
        denseState.numerical().forget(targetVariable);
        return;
    }

    const auto* sourceValue = SVFUtil::dyn_cast<ValVar>(source);
    if (exactMathematicalCopy && sourceValue &&
            adapter_.contains(*sourceValue))
    {
        const AD::Variable sourceVariable = adapter_.variable(*sourceValue);
        ensureVariable(denseState, sourceVariable);
        denseState.numerical().assign(
            targetVariable, AD::LinearExpression(sourceVariable));
        constrainInterval(denseState, targetVariable, result.getInterval());
    }
    else
    {
        assignInterval(denseState, targetVariable, result.getInterval());
    }
    rebuildCompatibilityProjection(node);
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::updateDomainOnCopy(
    const CopyStmt* copy)
{
    const bool exact = copy->getCopyKind() == CopyStmt::COPYVAL ||
                       copy->getCopyKind() == CopyStmt::SEXT;
    updateDomainCopyValue(copy->getICFGNode(), copy->getLHSVar(),
                              copy->getRHSVar(), exact);
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::
synchronizeDomainFromIntervalView(const ICFGNode* node)
{
    DenseState& denseState = ensureState(node);
    const IntervalState& projection = abstractTrace[node];

    for (const auto& [id, value] : projection.getVarToVal())
    {
        const auto* scalar =
            SVFUtil::dyn_cast<ValVar>(svfir->getGNode(id));
        if (!scalar || !adapter_.contains(*scalar))
            continue;
        const AD::Variable variable = adapter_.variable(*scalar);
        AbstractValue current = projectValue(denseState, variable);
        if (scalar->isPointer())
            current.interval = IntervalValue::bottom();
        if (!current.equals(value))
            assignValue(denseState, variable, value);
        definedVariables_[node].insert(variable);
    }

    for (const auto& [objectId, value] : projection.getLocToVal())
    {
        const auto* object =
            SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(objectId));
        if (!object || !adapter_.contains(*object))
            continue;
        const AD::Variable content = adapter_.contentVariable(*object);
        if (!projectValue(denseState, content).equals(value))
            assignValue(denseState, content, value);
        definedVariables_[node].insert(content);
    }

    for (NodeID address : projection.getFreedAddrs())
    {
        const NodeID objectId = address & FlippedAddressMask;
        const auto* object =
            SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(objectId));
        if (object && adapter_.contains(*object))
            denseState.lifetimes().release(adapter_.location(*object));
    }
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::assignValue(
    DenseState& denseState, AD::Variable variable,
    const AbstractValue& value)
{
    ensureVariable(denseState, variable);
    if (value.isInterval())
        assignInterval(denseState, variable, value.getInterval());
    else
        denseState.numerical().forget(variable);

    AD::AddressSet addresses = AD::AddressSet::bottom();
    for (u32_t address : value.getAddrs())
    {
        const NodeID objectId = address & FlippedAddressMask;
        const auto* object = SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(objectId));
        if (object && adapter_.contains(*object))
            addresses.insert(adapter_.location(*object));
    }
    denseState.addresses().assign(variable, std::move(addresses));
}

template <typename NumericalStateT>
AbstractValue DenseAbstractInterpretation<NumericalStateT>::projectValue(
    const DenseState& denseState, AD::Variable variable) const
{
    if (!denseState.numerical().environment().contains(variable))
        return AbstractValue(IntervalValue::top());
    AbstractValue result(projectInterval(denseState.numerical().bound(variable)));
    const AD::AddressSet addresses = denseState.addresses().addressesOf(variable);
    if (addresses.isTop())
    {
        result.getAddrs().insert(BlackHoleObjAddr);
        return result;
    }
    for (AD::Location location : addresses.locations())
    {
        const ObjVar& object = adapter_.object(location);
        result.getAddrs().insert(
            IntervalState::getVirtualMemAddress(object.getId()));
    }
    return result;
}

template <typename NumericalStateT>
const AbstractValue& DenseAbstractInterpretation<NumericalStateT>::getAbsValue(
    const ValVar* var, const ICFGNode* node)
{
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(var))
    {
        abstractTrace[node][var->getId()] =
            IntervalValue(integer->getSExtValue());
        return abstractTrace[node][var->getId()];
    }
    if (!adapter_.contains(*var))
        return AbstractInterpretation::getAbsValue(var, node);

    definedVariables_[node].insert(adapter_.variable(*var));
    AbstractValue value = projectValue(ensureState(node), adapter_.variable(*var));
    if (var->isPointer())
        value.interval = IntervalValue::bottom();
    abstractTrace[node][var->getId()] = std::move(value);
    return abstractTrace[node][var->getId()];
}

template <typename NumericalStateT>
const AbstractValue& DenseAbstractInterpretation<NumericalStateT>::getAbsValue(
    const ObjVar* var, const ICFGNode* node)
{
    if (!adapter_.contains(*var))
        return AbstractInterpretation::getAbsValue(var, node);
    definedVariables_[node].insert(adapter_.contentVariable(*var));
    AbstractValue value =
        projectValue(ensureState(node), adapter_.contentVariable(*var));
    const u32_t address = IntervalState::getVirtualMemAddress(var->getId());
    abstractTrace[node].store(address, value);
    return abstractTrace[node].load(address);
}

template <typename NumericalStateT>
const AbstractValue& DenseAbstractInterpretation<NumericalStateT>::getAbsValue(
    const SVFVar* var, const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getAbsValue(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getAbsValue(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

template <typename NumericalStateT>
bool DenseAbstractInterpretation<NumericalStateT>::hasAbsValue(
    const ValVar* var, const ICFGNode* node) const
{
    if (SVFUtil::isa<ConstIntValVar>(var))
        return true;
    if (denseTrace_.count(node) == 0 || !adapter_.contains(*var))
        return false;
    const auto definitions = definedVariables_.find(node);
    return definitions != definedVariables_.end() &&
           definitions->second.count(adapter_.variable(*var)) != 0;
}

template <typename NumericalStateT>
bool DenseAbstractInterpretation<NumericalStateT>::hasAbsValue(
    const ObjVar* var, const ICFGNode* node) const
{
    if (denseTrace_.count(node) == 0 || !adapter_.contains(*var))
        return false;
    const auto definitions = definedVariables_.find(node);
    return definitions != definedVariables_.end() &&
           definitions->second.count(adapter_.contentVariable(*var)) != 0;
}

template <typename NumericalStateT>
bool DenseAbstractInterpretation<NumericalStateT>::hasAbsValue(
    const SVFVar* var, const ICFGNode* node) const
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return hasAbsValue(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return hasAbsValue(value, node);
    return false;
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::updateAbsValue(
    const ValVar* var, const AbstractValue& value, const ICFGNode* node)
{
    if (adapter_.contains(*var))
    {
        assignValue(ensureState(node), adapter_.variable(*var), value);
        definedVariables_[node].insert(adapter_.variable(*var));
    }
    abstractTrace[node][var->getId()] = value;
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::updateAbsValue(
    const ObjVar* var, const AbstractValue& value, const ICFGNode* node)
{
    if (adapter_.contains(*var))
    {
        assignValue(ensureState(node), adapter_.contentVariable(*var), value);
        definedVariables_[node].insert(adapter_.contentVariable(*var));
    }
    abstractTrace[node].store(
        IntervalState::getVirtualMemAddress(var->getId()), value);
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::updateAbsValue(
    const SVFVar* var, const AbstractValue& value, const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        updateAbsValue(object, value, node);
    else if (const auto* scalar = SVFUtil::dyn_cast<ValVar>(var))
        updateAbsValue(scalar, value, node);
    else
        throw std::invalid_argument("unsupported SVF variable kind");
}

template <typename NumericalStateT>
AbstractValue DenseAbstractInterpretation<NumericalStateT>::loadValue(
    const ValVar* pointer, const ICFGNode* node)
{
    if (!adapter_.contains(*pointer))
        return AbstractInterpretation::loadValue(pointer, node);
    const DenseState& denseState = state(node);
    const AD::AddressSet pointees =
        denseState.addresses().addressesOf(adapter_.variable(*pointer));
    if (pointees.isTop())
        return AbstractValue(IntervalValue::top());

    AbstractValue result;
    for (AD::Location location : pointees.locations())
    {
        if (denseState.lifetimes().mayBeFreed(location))
        {
            result.join_with(AbstractValue(IntervalValue::top()));
            continue;
        }
        if (denseState.memoryLayout().contains(location))
        {
            result.join_with(projectValue(
                denseState, denseState.memoryLayout().contentOf(location)));
        }
    }
    return result;
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::storeValue(
    const ValVar* pointer, const AbstractValue& value,
    const ICFGNode* node)
{
    if (!adapter_.contains(*pointer))
    {
        AbstractInterpretation::storeValue(pointer, value, node);
        return;
    }
    DenseState& denseState = ensureState(node);
    const AD::AddressSet pointees =
        denseState.addresses().addressesOf(adapter_.variable(*pointer));
    const bool strong = pointees.isSingleton();
    auto write = [&](AD::Location location)
    {
        if (!denseState.memoryLayout().contains(location))
            return;
        const AD::Variable content =
            denseState.memoryLayout().contentOf(location);
        if (strong)
        {
            assignValue(denseState, content, value);
            definedVariables_[node].insert(content);
            return;
        }
        AbstractValue joined = projectValue(denseState, content);
        joined.join_with(value);
        assignValue(denseState, content, joined);
        definedVariables_[node].insert(content);
    };

    if (pointees.isTop())
    {
        for (const auto& [location, content] :
                denseState.memoryLayout().cells())
        {
            (void)content;
            write(location);
        }
    }
    else
    {
        for (AD::Location location : pointees.locations())
            write(location);
    }
    rebuildCompatibilityProjection(node);
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::assumeBranch(
    const IntraCFGEdge* edge, DenseState& denseState) const
{
    const SVFVar* condition = edge->getCondition();
    if (!condition || condition->getInEdges().empty())
        return;
    const auto* comparison = SVFUtil::dyn_cast<CmpStmt>(
        *condition->getInEdges().begin());
    if (!comparison)
    {
        const auto* value = SVFUtil::dyn_cast<ValVar>(condition);
        if (!value || !adapter_.contains(*value))
            return;
        denseState.assume(AD::equal(
            AD::LinearExpression(adapter_.variable(*value)),
            AD::LinearExpression(
                AD::Rational(edge->getSuccessorCondValue()))));
        return;
    }

    AD::ConstraintKind kind;
    if (!constraintKind(comparison->getPredicate(), kind))
        return;
    if (edge->getSuccessorCondValue() == 0)
        kind = negatePredicate(kind);

    auto operand = [&](const SVFVar* variable,
                       AD::LinearExpression& expression) -> bool
    {
        if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(variable))
        {
            expression = AD::LinearExpression(
                AD::Rational(integer->getSExtValue()));
            return true;
        }
        const auto* value = SVFUtil::dyn_cast<ValVar>(variable);
        if (!value || !adapter_.contains(*value))
            return false;
        expression = AD::LinearExpression(adapter_.variable(*value));
        return true;
    };

    AD::LinearExpression lhs;
    AD::LinearExpression rhs;
    if (!operand(comparison->getOpVar(0), lhs) ||
            !operand(comparison->getOpVar(1), rhs))
        return;
    denseState.assume(AD::LinearConstraint(lhs - rhs, kind));
}

template <typename NumericalStateT>
bool DenseAbstractInterpretation<NumericalStateT>::mergeStatesFromPredecessors(
    const ICFGNode* node)
{
    DenseState merged = bottomState(node);
    IntervalState mergedProjection;
    std::set<AD::Variable> mergedDefinitions;
    bool hasFeasiblePredecessor = false;

    for (const ICFGEdge* edge : node->getInEdges())
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (denseTrace_.count(predecessor) == 0)
            continue;

        bool shouldMerge = false;
        const IntraCFGEdge* conditional =
            SVFUtil::dyn_cast<IntraCFGEdge>(edge);
        if (conditional)
        {
            shouldMerge = !conditional->getCondition() ||
                          isBranchEdgeFeasible(
                              conditional, abstractTrace[predecessor]);
        }
        else if (SVFUtil::isa<CallCFGEdge>(edge))
        {
            shouldMerge = true;
        }
        else if (SVFUtil::isa<RetCFGEdge>(edge))
        {
            shouldMerge = Options::HandleRecur() == TOP;
            if (!shouldMerge)
            {
                const auto* returnSite =
                    SVFUtil::dyn_cast<RetICFGNode>(node);
                shouldMerge = returnSite &&
                    denseTrace_.count(returnSite->getCallICFGNode()) != 0;
            }
        }
        if (!shouldMerge)
            continue;

        DenseState source = state(predecessor);
        source.changeEnvironment(adapter_.environment(node->getFun()));
        if (conditional && conditional->getCondition())
            assumeBranch(conditional, source);
        if (source.isBottom())
            continue;

        merged.joinWith(source);
        mergedProjection.joinWith(abstractTrace[predecessor]);
        const auto definitions = definedVariables_.find(predecessor);
        if (definitions != definedVariables_.end())
        {
            mergedDefinitions.insert(definitions->second.begin(),
                                     definitions->second.end());
        }
        hasFeasiblePredecessor = true;
    }

    if (!hasFeasiblePredecessor)
        return false;
    for (auto iterator = mergedDefinitions.begin();
            iterator != mergedDefinitions.end();)
    {
        if (!merged.numerical().environment().contains(*iterator))
            iterator = mergedDefinitions.erase(iterator);
        else
            ++iterator;
    }
    denseTrace_.insert_or_assign(node, std::move(merged));
    definedVariables_[node] = std::move(mergedDefinitions);
    abstractTrace[node] = std::move(mergedProjection);
    rebuildCompatibilityProjection(node);
    return true;
}

template <typename NumericalStateT>
void DenseAbstractInterpretation<NumericalStateT>::
rebuildCompatibilityProjection(const ICFGNode* node)
{
    DenseState& denseState = ensureState(node);
    IntervalState& projection = abstractTrace[node];
    for (const ValVar* value : adapter_.trackedValues())
    {
        if (value->getFunction() && value->getFunction() != node->getFun())
            continue;
        if (definedVariables_[node].count(adapter_.variable(*value)) == 0)
            continue;
        AbstractValue projected =
            projectValue(denseState, adapter_.variable(*value));
        if (value->isPointer())
            projected.interval = IntervalValue::bottom();
        projection[value->getId()] = std::move(projected);
    }
    for (const ObjVar* object : adapter_.trackedObjects())
    {
        if (definedVariables_[node].count(
                adapter_.contentVariable(*object)) == 0)
            continue;
        projection.store(
            IntervalState::getVirtualMemAddress(object->getId()),
            projectValue(denseState, adapter_.contentVariable(*object)));
    }
}

template class SVF::DenseAbstractInterpretation<AD::BoxState>;
template class SVF::DenseAbstractInterpretation<AD::OctagonState>;
