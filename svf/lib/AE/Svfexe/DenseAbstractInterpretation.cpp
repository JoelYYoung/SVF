//===- DenseAbstractInterpretation.cpp -- Domain-backed dense AE --------===//

#include "AE/Svfexe/DenseAbstractInterpretation.h"

#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace SVF
{

namespace AD = AbstractDomain;

namespace
{

std::vector<const ICFGEdge*> orderedIncomingEdges(const ICFGNode* node)
{
    std::vector<const ICFGEdge*> edges(node->getInEdges().begin(),
                                       node->getInEdges().end());
    std::sort(edges.begin(), edges.end(),
              [](const ICFGEdge* lhs, const ICFGEdge* rhs) {
                  return std::make_tuple(lhs->getSrcID(),
                                         lhs->getEdgeKindWithoutMask()) <
                         std::make_tuple(rhs->getSrcID(),
                                         rhs->getEdgeKindWithoutMask());
              });
    return edges;
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

bool constraintKind(u32_t predicate, AD::ConstraintKind& kind)
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

template <typename DenseStateT>
void alignEnvironments(DenseStateT& lhs, DenseStateT& rhs)
{
    if (lhs.numerical().environment() == rhs.numerical().environment())
        return;
    const AD::VariableEnvironment environment =
        lhs.numerical().environment().merge(rhs.numerical().environment());
    if (lhs.numerical().environment() != environment)
        lhs.changeEnvironment(environment);
    if (rhs.numerical().environment() != environment)
        rhs.changeEnvironment(environment);
}

} // namespace

template <typename NumericalDomainT>
DenseAbstractInterpretation<NumericalDomainT>::DenseAbstractInterpretation()
    : adapter_(*svfir)
{
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::runOnModule()
{
    AbstractInterpretation::runOnModule();
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::handleGlobalNode()
{
    const ICFGNode* node = icfg->getGlobalICFGNode();
    // The global ICFG node contains address initializers for local pointer
    // values from every function.  Growing a relational environment once per
    // initializer repeatedly rebuilds and normalizes the same state.  Batch
    // those result dimensions into the initial top state instead.
    std::vector<AD::VariableDeclaration> initialDeclarations;
    std::set<AD::Variable> initialVariables;
    auto addInitialValue = [&](const SVFVar* variable) {
        const auto* value = SVFUtil::dyn_cast<ValVar>(variable);
        if (!value || !adapter_.contains(*value))
            return;
        const AD::Variable symbol = adapter_.variable(*value);
        if (!adapter_.environment().contains(symbol) &&
            initialVariables.insert(symbol).second)
            initialDeclarations.push_back(adapter_.declaration(symbol));
    };
    for (const SVFStmt* statement : node->getSVFStmts())
    {
        if (const auto* assignment = SVFUtil::dyn_cast<AssignStmt>(statement))
            addInitialValue(assignment->getLHSVar());
        else if (const auto* multi =
                     SVFUtil::dyn_cast<MultiOpndStmt>(statement))
            addInitialValue(multi->getRes());
    }
    if (const auto* blackHole = SVFUtil::dyn_cast<ValVar>(
            svfir->getGNode(PAG::getPAG()->getBlkPtr())))
        addInitialValue(blackHole);
    const AD::VariableEnvironment initialEnvironment =
        adapter_.environment().add(std::move(initialDeclarations));
    denseTrace_.insert_or_assign(
        node, DenseState(makeNumericalTop(initialEnvironment),
                         adapter_.memoryLayout()));
    for (const SVFStmt* statement : node->getSVFStmts())
        handleSVFStatement(statement);

    if (const auto* variable = SVFUtil::dyn_cast<ValVar>(
            svfir->getGNode(PAG::getPAG()->getBlkPtr())))
        updateValue(variable, AD::Interval::top(), AD::AddressSet::top(), node);
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::initializeObjectValue(
    const ObjVar* object, AD::Interval& interval, AD::AddressSet& addresses,
    const ICFGNode* node)
{
    interval = AD::Interval::bottom();
    addresses = AD::AddressSet::bottom();
    DenseState& denseState = ensureState(node);
    if (adapter_.contains(*object))
        denseState.allocate(adapter_.location(*object));

    const BaseObjVar* base = PAG::getPAG()->getBaseObject(object->getId());
    if (base->isConstDataOrConstGlobal() || base->isConstantArray() ||
        base->isConstantStruct())
    {
        if (const auto* integer = SVFUtil::dyn_cast<ConstIntObjVar>(object))
            interval =
                AD::Interval::singleton(AD::Rational(integer->getSExtValue()));
        else if (const auto* floating =
                     SVFUtil::dyn_cast<ConstFPObjVar>(object))
            interval = AD::Interval::singleton(
                AD::Rational(std::to_string(floating->getFPValue())));
        else if (SVFUtil::isa<ConstNullPtrObjVar>(object))
            addresses = AD::AddressSet::singleton(AD::Location::null());
        else if (!SVFUtil::isa<GlobalObjVar>(object))
            interval = AD::Interval::top();
        if (!interval.isBottom() || !addresses.isBottom())
            return;
    }
    if (adapter_.contains(*object))
        addresses = AD::AddressSet::singleton(adapter_.location(*object));
    else
        addresses = AD::AddressSet::top();
}

template <typename NumericalDomainT>
const AbstractDomain::AbstractDomain& DenseAbstractInterpretation<
    NumericalDomainT>::getAbstractState(const ICFGNode* node) const
{
    return state(node);
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::hasAbsState(
    const ICFGNode* node) const
{
    return denseTrace_.count(node) != 0;
}

template <typename NumericalDomainT>
AD::Location DenseAbstractInterpretation<NumericalDomainT>::locationOf(
    const ObjVar* object) const
{
    return object && adapter_.contains(*object) ? adapter_.location(*object)
                                                : AD::Location::null();
}

template <typename NumericalDomainT>
const ObjVar* DenseAbstractInterpretation<NumericalDomainT>::objectAt(
    AD::Location location) const
{
    return location.isNull() ? nullptr : &adapter_.object(location);
}

template <typename NumericalDomainT>
typename DenseAbstractInterpretation<NumericalDomainT>::DenseState
DenseAbstractInterpretation<NumericalDomainT>::topState(
    const ICFGNode* node) const
{
    return DenseState(
        makeNumericalTop(adapter_.environment(node ? node->getFun() : nullptr)),
        adapter_.memoryLayout());
}

template <typename NumericalDomainT>
typename DenseAbstractInterpretation<NumericalDomainT>::DenseState
DenseAbstractInterpretation<NumericalDomainT>::bottomState(
    const ICFGNode* node) const
{
    return DenseState(makeNumericalBottom(adapter_.environment(
                          node ? node->getFun() : nullptr)),
                      adapter_.memoryLayout());
}

template <typename NumericalDomainT>
NumericalDomainT DenseAbstractInterpretation<NumericalDomainT>::
    makeNumericalTop(const AD::VariableEnvironment& environment) const
{
    if constexpr (std::is_same_v<NumericalDomainT, AD::BoxDomain>)
        return AD::BoxDomain::top(environment);
    else
        return NumericalDomainT::top(environment);
}

template <typename NumericalDomainT>
NumericalDomainT DenseAbstractInterpretation<NumericalDomainT>::
    makeNumericalBottom(const AD::VariableEnvironment& environment) const
{
    if constexpr (std::is_same_v<NumericalDomainT, AD::BoxDomain>)
        return AD::BoxDomain::bottom(environment);
    else
        return NumericalDomainT::bottom(environment);
}

template <typename NumericalDomainT>
typename DenseAbstractInterpretation<NumericalDomainT>::DenseState&
DenseAbstractInterpretation<NumericalDomainT>::ensureState(const ICFGNode* node)
{
    auto iterator = denseTrace_.find(node);
    if (iterator == denseTrace_.end())
        iterator = denseTrace_.emplace(node, topState(node)).first;
    return iterator->second;
}

template <typename NumericalDomainT>
const typename DenseAbstractInterpretation<NumericalDomainT>::DenseState&
DenseAbstractInterpretation<NumericalDomainT>::state(const ICFGNode* node) const
{
    const auto iterator = denseTrace_.find(node);
    if (iterator == denseTrace_.end())
        throw std::out_of_range("no dense abstract state for ICFG node");
    return iterator->second;
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::resetAbstractState(
    const ICFGNode* node)
{
    denseTrace_.insert_or_assign(node, topState(node));
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::copyAbstractState(
    const ICFGNode* source, const ICFGNode* destination)
{
    DenseState copy = state(source);
    const AD::VariableEnvironment& destinationEnvironment =
        adapter_.environment(destination->getFun());
    if (copy.numerical().environment() == destinationEnvironment)
    {
        denseTrace_.insert_or_assign(destination, std::move(copy));
        return;
    }

    copy.changeEnvironment(destinationEnvironment);
    denseTrace_.insert_or_assign(destination, std::move(copy));
}

template <typename NumericalDomainT>
std::unique_ptr<AbstractDomain::AbstractDomain> DenseAbstractInterpretation<
    NumericalDomainT>::cloneAbstractState(const ICFGNode* node) const
{
    return state(node).clone();
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::isAbstractStateEquivalent(
    const ICFGNode* node, const AbstractDomain::AbstractDomain& snapshot) const
{
    DenseState current = state(node);
    DenseState previous = static_cast<const DenseState&>(snapshot);
    alignEnvironments(current, previous);
    return current.isEquivalentTo(previous) ==
           AbstractDomain::CheckResult::True;
}

template <typename NumericalDomainT>
std::unique_ptr<AbstractDomain::AbstractDomain> DenseAbstractInterpretation<
    NumericalDomainT>::cloneCycleHeadState(const ICFGCycleWTO* cycle)
{
    return cloneAbstractState(cycle->head()->getICFGNode());
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::widenCycleState(
    const AbstractDomain::AbstractDomain& previous,
    const AbstractDomain::AbstractDomain& current, const ICFGCycleWTO* cycle)
{
    DenseState previousDense = static_cast<const DenseState&>(previous);
    DenseState currentDense = static_cast<const DenseState&>(current);
    alignEnvironments(previousDense, currentDense);
    DenseState next = previousDense;
    next.widenWith(currentDense);
    const bool fixpoint =
        next.isEquivalentTo(previousDense) == AbstractDomain::CheckResult::True;
    const ICFGNode* head = cycle->head()->getICFGNode();
    denseTrace_.insert_or_assign(head, std::move(next));
    return fixpoint;
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::narrowCycleState(
    const AbstractDomain::AbstractDomain& previous,
    const AbstractDomain::AbstractDomain& current, const ICFGCycleWTO* cycle)
{
    const ICFGNode* head = cycle->head()->getICFGNode();
    if (!shouldApplyNarrowing(head->getFun()))
        return true;
    DenseState previousDense = static_cast<const DenseState&>(previous);
    DenseState currentDense = static_cast<const DenseState&>(current);
    alignEnvironments(previousDense, currentDense);
    // Sparse transfers may materialize a new MemorySSA/cycle facet during the
    // descending phase. Enforce narrowing's generic next <= current contract.
    // The normal descending path already satisfies that contract. Avoid
    // rebuilding and closing a relational meet when the lattice check proves
    // that the meet would be exactly currentDense. False and Unknown retain
    // the original conservative meet.
    if (currentDense.isSubsetOf(previousDense) != AD::CheckResult::True)
        currentDense.meetWith(previousDense);
    DenseState next = previousDense;
    next.narrowWith(currentDense);
    const bool fixpoint =
        next.isEquivalentTo(previousDense) == AbstractDomain::CheckResult::True;
    if (!fixpoint)
        denseTrace_.insert_or_assign(head, std::move(next));
    return fixpoint;
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::ensureVariable(
    DenseState& denseState, AD::Variable variable) const
{
    if (denseState.numerical().environment().contains(variable))
        return;
    denseState.changeEnvironment(denseState.numerical().environment().add(
        {adapter_.declaration(variable)}));
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::assignInterval(
    DenseState& denseState, AD::Variable variable, const AD::Interval& interval)
{
    ensureVariable(denseState, variable);
    denseState.numerical().forget(variable);
    constrainInterval(denseState, variable, interval);
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::constrainInterval(
    DenseState& denseState, AD::Variable variable, const AD::Interval& interval)
{
    if (interval.isBottom())
        return;

    AD::LinearConstraintSet constraints;
    AD::LinearExpression expression(variable);
    if (interval.lower().isFinite())
    {
        constraints.emplace_back(
            expression - AD::LinearExpression(interval.lower().value()),
            interval.lower().isStrict() ? AD::ConstraintKind::GreaterThan
                                        : AD::ConstraintKind::GreaterEqual);
    }
    if (interval.upper().isFinite())
    {
        constraints.emplace_back(
            expression - AD::LinearExpression(interval.upper().value()),
            interval.upper().isStrict() ? AD::ConstraintKind::LessThan
                                        : AD::ConstraintKind::LessEqual);
    }
    denseState.numerical().assumeAll(constraints);
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::initializeDomainState(
    const ICFGNode* node)
{
    (void)ensureState(node);
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::assignValue(
    DenseState& denseState, AD::Variable variable, const AD::Interval& interval,
    const AD::AddressSet& addresses)
{
    ensureVariable(denseState, variable);
    if (!interval.isBottom())
        assignInterval(denseState, variable, interval);
    else
        denseState.numerical().forget(variable);
    denseState.addresses().assign(variable, addresses);
    denseState.shapes().assign(variable, !interval.isBottom());
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::materializeValue(
    DenseState&, const ValVar*, const ICFGNode*)
{
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::forgetValue(
    DenseState& denseState, AD::Variable variable) const
{
    if (!denseState.numerical().environment().contains(variable))
        return;
    denseState.numerical().forget(variable);
    // This helper removes an AE value; it does not model an unknown pointer.
    // AddressDomain::forget means address-top and would retain an explicit map
    // entry for every purged sparse scalar.
    denseState.addresses().assign(variable, AD::AddressSet::bottom());
    denseState.shapes().forget(variable);
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::forgetScalarValues(
    DenseState& denseState) const
{
    for (const AD::VariableDeclaration& declaration :
         denseState.numerical().environment().variables())
    {
        if (adapter_.value(declaration.variable))
            forgetValue(denseState, declaration.variable);
    }
}

template <typename NumericalDomainT>
AD::Interval DenseAbstractInterpretation<NumericalDomainT>::getInterval(
    const ValVar* var, const ICFGNode* node)
{
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(var))
        return AD::Interval::singleton(AD::Rational(integer->getSExtValue()));
    if (!adapter_.contains(*var))
        return AD::Interval::top();

    DenseState& denseState = ensureState(node);
    const AD::Variable variable = adapter_.variable(*var);
    if (!denseState.shapes().isDefined(variable))
        assignValue(denseState, variable, AD::Interval::top(),
                    AD::AddressSet::bottom());
    if (var->isPointer())
        return AD::Interval::bottom();
    return denseState.shapes().hasNumeric(variable)
               ? denseState.numerical().bound(variable)
               : AD::Interval::bottom();
}

template <typename NumericalDomainT>
AD::Interval DenseAbstractInterpretation<NumericalDomainT>::getInterval(
    const ObjVar* var, const ICFGNode* node)
{
    if (!adapter_.contains(*var))
        return AD::Interval::bottom();
    DenseState& denseState = ensureState(node);
    const AD::Variable content = adapter_.contentVariable(*var);
    if (!denseState.shapes().isDefined(content))
        assignValue(denseState, content, AD::Interval::bottom(),
                    AD::AddressSet::bottom());
    return denseState.shapes().hasNumeric(content)
               ? denseState.numerical().bound(content)
               : AD::Interval::bottom();
}

template <typename NumericalDomainT>
AD::Interval DenseAbstractInterpretation<NumericalDomainT>::getInterval(
    const SVFVar* var, const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getInterval(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getInterval(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

template <typename NumericalDomainT>
AD::AddressSet DenseAbstractInterpretation<NumericalDomainT>::getAddressSet(
    const ValVar* var, const ICFGNode* node)
{
    if (!adapter_.contains(*var))
        return AD::AddressSet::top();
    DenseState& denseState = ensureState(node);
    const AD::Variable variable = adapter_.variable(*var);
    if (!denseState.shapes().isDefined(variable))
        assignValue(denseState, variable, AD::Interval::top(),
                    AD::AddressSet::bottom());
    return denseState.addresses().addressSet(variable);
}

template <typename NumericalDomainT>
AD::AddressSet DenseAbstractInterpretation<NumericalDomainT>::getAddressSet(
    const ObjVar* var, const ICFGNode* node)
{
    if (!adapter_.contains(*var))
        return AD::AddressSet::bottom();
    DenseState& denseState = ensureState(node);
    const AD::Variable content = adapter_.contentVariable(*var);
    if (!denseState.shapes().isDefined(content))
        assignValue(denseState, content, AD::Interval::bottom(),
                    AD::AddressSet::bottom());
    return denseState.addresses().addressSet(content);
}

template <typename NumericalDomainT>
AD::AddressSet DenseAbstractInterpretation<NumericalDomainT>::getAddressSet(
    const SVFVar* var, const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getAddressSet(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getAddressSet(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::hasAbsValue(
    const ValVar* var, const ICFGNode* node) const
{
    if (SVFUtil::isa<ConstIntValVar>(var))
        return true;
    if (denseTrace_.count(node) == 0 || !adapter_.contains(*var))
        return false;
    return state(node).shapes().isDefined(adapter_.variable(*var));
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::hasAbsValue(
    const ObjVar* var, const ICFGNode* node) const
{
    if (denseTrace_.count(node) == 0 || !adapter_.contains(*var))
        return false;
    return state(node).shapes().isDefined(adapter_.contentVariable(*var));
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::hasAbsValue(
    const SVFVar* var, const ICFGNode* node) const
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return hasAbsValue(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return hasAbsValue(value, node);
    return false;
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::updateValue(
    const ValVar* var, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (adapter_.contains(*var))
        assignValue(ensureState(node), adapter_.variable(*var), interval,
                    addresses);
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::updateValue(
    const ObjVar* var, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (adapter_.contains(*var))
        assignValue(ensureState(node), adapter_.contentVariable(*var), interval,
                    addresses);
}

template <typename NumericalDomainT>
AD::Interval DenseAbstractInterpretation<NumericalDomainT>::getMemoryInterval(
    AD::Location location, const ICFGNode* node)
{
    if (location.isNull())
        return AD::Interval::bottom();
    return getInterval(&adapter_.object(location), node);
}

template <typename NumericalDomainT>
AD::AddressSet DenseAbstractInterpretation<
    NumericalDomainT>::getMemoryAddressSet(AD::Location location,
                                           const ICFGNode* node)
{
    if (location.isNull())
        return AD::AddressSet::bottom();
    return getAddressSet(&adapter_.object(location), node);
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::hasMemoryValue(
    AD::Location location, const ICFGNode* node) const
{
    return !location.isNull() && hasAbsValue(&adapter_.object(location), node);
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::updateMemoryValue(
    AD::Location location, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (!location.isNull())
        updateValue(&adapter_.object(location), interval, addresses, node);
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::markFreedMemory(
    AD::Location location, const ICFGNode* node)
{
    if (!location.isNull())
        ensureState(node).lifetimes().release(location);
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::isFreedMemory(
    AD::Location location, const ICFGNode* node) const
{
    if (denseTrace_.count(node) == 0 || location.isNull())
        return false;
    return state(node).lifetimes().mayBeFreed(location);
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::updateValue(
    const SVFVar* var, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        updateValue(object, interval, addresses, node);
    else if (const auto* scalar = SVFUtil::dyn_cast<ValVar>(var))
        updateValue(scalar, interval, addresses, node);
    else
        throw std::invalid_argument("unsupported SVF variable kind");
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::loadValue(
    const ValVar* pointer, AD::Interval& interval, AD::AddressSet& addresses,
    const ICFGNode* node)
{
    if (!adapter_.contains(*pointer))
    {
        AbstractInterpretation::loadValue(pointer, interval, addresses, node);
        return;
    }
    DenseState& denseState = ensureState(node);
    materializeValue(denseState, pointer, node);
    const AD::AddressSet pointees =
        denseState.addresses().addressSet(adapter_.variable(*pointer));
    if (pointees.isTop())
    {
        interval = AD::Interval::top();
        addresses = AD::AddressSet::top();
        return;
    }

    interval = AD::Interval::bottom();
    addresses = AD::AddressSet::bottom();
    for (AD::Location location : pointees.locations())
    {
        if (denseState.lifetimes().mayBeFreed(location))
        {
            interval.joinWith(AD::Interval::top());
            addresses.joinWith(AD::AddressSet::top());
            continue;
        }
        if (denseState.memoryLayout().contains(location))
        {
            const AD::Variable content =
                denseState.memoryLayout().contentOf(location);
            if (denseState.shapes().hasNumeric(content))
                interval.joinWith(denseState.numerical().bound(content));
            addresses.joinWith(denseState.addresses().addressSet(content));
        }
    }
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::storeValue(
    const ValVar* pointer, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (!adapter_.contains(*pointer))
    {
        AbstractInterpretation::storeValue(pointer, interval, addresses, node);
        return;
    }
    DenseState& denseState = ensureState(node);
    materializeValue(denseState, pointer, node);
    const AD::AddressSet pointees =
        denseState.addresses().addressSet(adapter_.variable(*pointer));
    const bool strong = pointees.isSingleton();
    auto write = [&](AD::Location location) {
        if (!denseState.memoryLayout().contains(location))
            return;
        const AD::Variable content =
            denseState.memoryLayout().contentOf(location);
        if (strong)
        {
            assignValue(denseState, content, interval, addresses);
            return;
        }
        AD::Interval joinedInterval =
            denseState.shapes().hasNumeric(content)
                ? denseState.numerical().bound(content)
                : AD::Interval::bottom();
        AD::AddressSet joinedAddresses =
            denseState.addresses().addressSet(content);
        joinedInterval.joinWith(interval);
        joinedAddresses.joinWith(addresses);
        assignValue(denseState, content, joinedInterval, joinedAddresses);
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
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::assumeBranch(
    const IntraCFGEdge* edge, DenseState& denseState)
{
    const SVFVar* condition = edge->getCondition();
    if (!condition || condition->getInEdges().empty())
        return;
    const auto* comparison =
        SVFUtil::dyn_cast<CmpStmt>(*condition->getInEdges().begin());
    if (!comparison)
    {
        const auto* value = SVFUtil::dyn_cast<ValVar>(condition);
        if (!value || !adapter_.contains(*value))
            return;
        materializeValue(denseState, value, edge->getSrcNode());
        ensureVariable(denseState, adapter_.variable(*value));
        denseState.assume(AD::equal(
            AD::LinearExpression(adapter_.variable(*value)),
            AD::LinearExpression(AD::Rational(edge->getSuccessorCondValue()))));
        return;
    }

    AD::ConstraintKind kind;
    if (!constraintKind(comparison->getPredicate(), kind))
        return;
    if (edge->getSuccessorCondValue() == 0)
        kind = negatePredicate(kind);

    auto operand = [&](const SVFVar* variable,
                       AD::LinearExpression& expression) -> bool {
        if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(variable))
        {
            expression =
                AD::LinearExpression(AD::Rational(integer->getSExtValue()));
            return true;
        }
        const auto* value = SVFUtil::dyn_cast<ValVar>(variable);
        if (!value || !adapter_.contains(*value))
            return false;
        materializeValue(denseState, value, edge->getSrcNode());
        ensureVariable(denseState, adapter_.variable(*value));
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

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::mergeStatesFromPredecessors(
    const ICFGNode* node)
{
    DenseState merged = bottomState(node);
    bool hasFeasiblePredecessor = false;

    for (const ICFGEdge* edge : orderedIncomingEdges(node))
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (denseTrace_.count(predecessor) == 0)
            continue;

        bool shouldMerge = false;
        const IntraCFGEdge* conditional = SVFUtil::dyn_cast<IntraCFGEdge>(edge);
        if (conditional)
            shouldMerge = true;
        else if (SVFUtil::isa<CallCFGEdge>(edge))
        {
            shouldMerge = true;
        }
        else if (SVFUtil::isa<RetCFGEdge>(edge))
        {
            shouldMerge = Options::HandleRecur() == TOP;
            if (!shouldMerge)
            {
                const auto* returnSite = SVFUtil::dyn_cast<RetICFGNode>(node);
                shouldMerge =
                    returnSite &&
                    denseTrace_.count(returnSite->getCallICFGNode()) != 0;
            }
        }
        if (!shouldMerge)
            continue;

        DenseState source = state(predecessor);
        const AD::VariableEnvironment& destinationEnvironment =
            adapter_.environment(node->getFun());
        if (source.numerical().environment() != destinationEnvironment)
            source.changeEnvironment(destinationEnvironment);
        if (conditional && conditional->getCondition())
        {
            assumeBranch(conditional, source);
            collectBranchRefinement(conditional, source);
        }
        if (source.isBottom())
            continue;

        // Branch refinement can materialize a condition variable that is not
        // present in the destination function's precomputed environment (for
        // example, a value returned across a call edge).  Join over the union
        // environment just as the other fixpoint comparison paths do.
        alignEnvironments(merged, source);
        merged.joinWith(source);
        hasFeasiblePredecessor = true;
    }

    if (!hasFeasiblePredecessor)
        return false;
    denseTrace_.insert_or_assign(node, std::move(merged));
    return true;
}

template <typename NumericalDomainT>
void DenseAbstractInterpretation<NumericalDomainT>::recordBranchRefinement(
    NodeID objectId, const AD::Interval& narrowed,
    AD::AbstractDomain& abstractState, const ICFGNode*, const ICFGNode*)
{
    const auto* object = SVFUtil::dyn_cast<ObjVar>(svfir->getGNode(objectId));
    if (!object || !adapter_.contains(*object))
        return;

    DenseState& denseState = static_cast<DenseState&>(abstractState);
    const AD::Variable content = adapter_.contentVariable(*object);
    if (!denseState.shapes().hasNumeric(content))
        return;
    AD::Interval refined = denseState.numerical().bound(content);
    refined.meetWith(narrowed);
    assignValue(denseState, content, refined, AD::AddressSet::bottom());
}

template <typename NumericalDomainT>
bool DenseAbstractInterpretation<NumericalDomainT>::isBranchEdgeFeasibleAt(
    const IntraCFGEdge* edge, const ICFGNode* predecessor)
{
    DenseState candidate = state(predecessor);
    assumeBranch(edge, candidate);
    return !candidate.isBottom();
}

#ifndef SVF_DENSE_AE_SUPPRESS_EXPLICIT_INSTANTIATIONS
template class DenseAbstractInterpretation<AD::BoxDomain>;
#endif

} // namespace SVF
