//===- AbstractStateManager.cpp -- AE domain projection helpers --------===//

#include "AE/Svfexe/AbstractInterpretation.h"

#include "AE/Core/ConvexPolyhedraDomain.h"
#include "AE/Core/OctagonDomain.h"
#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace SVF
{

namespace AD = AbstractDomain;

namespace
{

s64_t finiteEndpoint(const AD::Bound& bound, s64_t fallback)
{
    if (!bound.isFinite())
        return fallback;
    try
    {
        return bound.value().toInt64();
    }
    catch (const std::exception&)
    {
        return fallback;
    }
}

AD::Interval finiteInterval(s64_t lower, s64_t upper)
{
    return AD::Interval::closed(AD::Rational(lower), AD::Rational(upper));
}

bool constantInterval(const ValVar* value, AD::Interval& interval)
{
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(value))
    {
        interval =
            AD::Interval::singleton(AD::Rational(integer->getSExtValue()));
        return true;
    }
    if (const auto* floating = SVFUtil::dyn_cast<ConstFPValVar>(value))
    {
        interval = SVFIRAdapter::floatingConstant(floating->getFPValue());
        return true;
    }
    return false;
}

} // namespace

const AD::AbstractDomain* AbstractInterpretation::getScalarAbstractState() const
{
    return nullptr;
}

void AbstractInterpretation::finalizeAbstractState(const ICFGNode*) {}

void AbstractInterpretation::updateInterval(const SVFVar* variable,
        const AD::Interval& interval,
        const ICFGNode* node)
{
    updateValue(variable, interval, AD::AddressSet::bottom(), node);
}

void AbstractInterpretation::updateAddressSet(const SVFVar* variable,
        const AD::AddressSet& addresses,
        const ICFGNode* node)
{
    updateValue(variable, AD::Interval::bottom(), addresses, node);
}

AD::Interval AbstractInterpretation::getGepElementIndex(const GepStmt* gep)
{
    const ICFGNode* node = gep->getICFGNode();
    if (gep->isConstantOffset())
        return AD::Interval::singleton(
                   AD::Rational(static_cast<s64_t>(gep->accumulateConstantOffset())));

    AD::Interval result = AD::Interval::singleton(AD::Rational());
    for (int index =
                static_cast<int>(gep->getOffsetVarAndGepTypePairVec().size()) - 1;
            index >= 0; --index)
    {
        const ValVar* variable =
            gep->getOffsetVarAndGepTypePairVec()[index].first;
        const SVFType* type =
            gep->getOffsetVarAndGepTypePairVec()[index].second;

        s64_t lower = 0;
        s64_t upper = 0;
        if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(variable))
        {
            lower = upper = integer->getSExtValue();
        }
        else
        {
            const AD::Interval value = getInterval(variable, node);
            // Original treats an uninitialized index as zero, not as an
            // unknown range spanning every field. Keep this AE policy apart
            // from the domain's Top/Bottom and finite-endpoint semantics.
            if (!value.isBottom())
            {
                lower = finiteEndpoint(value.lower(), 0);
                upper = finiteEndpoint(value.upper(), Options::MaxFieldLimit());
            }
        }

        if (SVFUtil::isa<SVFPointerType>(type))
        {
            const u32_t elements = gep->getAccessPath().getElementNum(
                                       gep->getAccessPath().gepSrcPointeeType());
            lower = (double)Options::MaxFieldLimit() / elements < lower
                    ? Options::MaxFieldLimit()
                    : lower * elements;
            upper = (double)Options::MaxFieldLimit() / elements < upper
                    ? Options::MaxFieldLimit()
                    : upper * elements;
        }
        else if (Options::ModelArrays())
        {
            const std::vector<u32_t>& flattened =
                PAG::getPAG()->getTypeInfo(type)->getFlattenedElemIdxVec();
            if (flattened.empty() ||
                    upper >= static_cast<APOffset>(flattened.size()) || lower < 0)
            {
                lower = upper = 0;
            }
            else
            {
                lower = PAG::getPAG()->getFlattenedElemIdx(type, lower);
                upper = PAG::getPAG()->getFlattenedElemIdx(type, upper);
            }
        }
        else
        {
            lower = upper = 0;
        }
        result = AD::add(result, finiteInterval(lower, upper));
    }
    result.meetWith(
        finiteInterval(0, static_cast<s64_t>(Options::MaxFieldLimit())));
    return result.isBottom() ? AD::Interval::singleton(AD::Rational()) : result;
}

AD::Interval AbstractInterpretation::getGepByteOffset(const GepStmt* gep)
{
    const ICFGNode* node = gep->getICFGNode();
    if (gep->isConstantOffset())
        return AD::Interval::singleton(AD::Rational(
                                           static_cast<s64_t>(gep->accumulateConstantByteOffset())));

    AD::Interval result = AD::Interval::singleton(AD::Rational());
    for (int index =
                static_cast<int>(gep->getOffsetVarAndGepTypePairVec().size()) - 1;
            index >= 0; --index)
    {
        const ValVar* variable =
            gep->getOffsetVarAndGepTypePairVec()[index].first;
        const SVFType* type =
            gep->getOffsetVarAndGepTypePairVec()[index].second;

        if (SVFUtil::isa<SVFArrayType>(type) ||
                SVFUtil::isa<SVFPointerType>(type))
        {
            u32_t elementSize = 1;
            if (const auto* array = SVFUtil::dyn_cast<SVFArrayType>(type))
                elementSize = array->getTypeOfElement()->getByteSize();
            else
                elementSize =
                    gep->getAccessPath().gepSrcPointeeType()->getByteSize();

            s64_t lower = 0;
            s64_t upper = 0;
            if (const auto* integer =
                        SVFUtil::dyn_cast<ConstIntValVar>(variable))
            {
                lower = upper = integer->getSExtValue();
            }
            else
            {
                const AD::Interval value = getInterval(variable, node);
                lower = finiteEndpoint(value.lower(), 0);
                upper = finiteEndpoint(value.upper(),
                                       Options::MaxFieldLimit());
            }
            lower = std::max<s64_t>(0, lower);
            upper = std::max<s64_t>(0, upper);
            lower = (double)Options::MaxFieldLimit() / elementSize >= lower
                    ? lower * elementSize
                    : Options::MaxFieldLimit();
            upper = (double)Options::MaxFieldLimit() / elementSize >= upper
                    ? upper * elementSize
                    : Options::MaxFieldLimit();
            result = AD::add(result, finiteInterval(lower, upper));
        }
        else if (const auto* structure = SVFUtil::dyn_cast<SVFStructType>(type))
        {
            const s64_t offset =
                gep->getAccessPath().getStructFieldOffset(variable, structure);
            result =
                AD::add(result, AD::Interval::singleton(AD::Rational(offset)));
        }
        else
        {
            throw std::invalid_argument(
                "GEP type pair must be array, pointer, or structure");
        }
    }
    return result;
}

AD::AddressSet AbstractInterpretation::getGepObjAddrs(
    const ValVar* pointer, const AD::Interval& offset, const ICFGNode* node)
{
    const AD::AddressSet bases = getAddressSet(pointer, node);
    if (offset.isBottom())
        return AD::AddressSet::bottom();

    // Empty is AE's no-target policy, not an unknown pointer. Flow-insensitive
    // points-to information must not resurrect targets killed by a later write.
    if (bases.isBottom())
        return AD::AddressSet::bottom();

    AD::AddressSet result = bases.hasUnknownObject()
                            ? AD::AddressSet::objectTop()
                            : AD::AddressSet::bottom();
    if (bases.mayContainRawAddress())
        result.joinWith(AD::AddressSet::rawTop());
    if (bases.hasUnknownObject())
        return result;

    auto integerEndpoint = [](const AD::Bound& bound,
                              bool lower) -> std::optional<APOffset>
    {
        if (!bound.isFinite())
        return std::nullopt;
        const AD::Rational integer =
        lower ? (bound.isStrict() ? bound.value().floor() + AD::Rational(1)
        : bound.value().ceil())
        : (bound.isStrict() ? bound.value().ceil() - AD::Rational(1)
        : bound.value().floor());
        try
        {
            return integer.toInt64();
            }
            catch (const std::exception&)
            {
                return std::nullopt;
            }
    };

    std::optional<APOffset> lower = integerEndpoint(offset.lower(), true);
    std::optional<APOffset> upper = integerEndpoint(offset.upper(), false);
    if (lower && upper && *lower > *upper)
        return AD::AddressSet::bottom();

    // A finite narrow range must retain its signed offsets: negative GEPs are
    // common in C++ vtable and subobject adjustment. SVFIR canonicalizes each
    // signed offset modulo the base object's field limit. For an unbounded or
    // wider range, enumerating one complete global field window covers every
    // possible canonical field without an unbounded loop.
    bool enumerateFieldUniverse = !lower || !upper;
    if (!enumerateFieldUniverse)
    {
        enumerateFieldUniverse =
            AD::Rational(*upper) - AD::Rational(*lower) >
            AD::Rational(static_cast<s64_t>(Options::MaxFieldLimit()));
    }
    if (enumerateFieldUniverse)
    {
        lower = 0;
        upper = static_cast<APOffset>(Options::MaxFieldLimit());
    }

    for (APOffset index = *lower;; ++index)
    {
        for (AD::Location base : bases)
        {
            if (base.isNull())
                continue;
            const ObjVar* object = objectAt(base);
            if (!object)
                continue;
            const NodeID gepObject =
                svfir->getGepObjVar(object->getId(), index);
            const auto* gepVariable =
                SVFUtil::dyn_cast<ObjVar>(svfir->getSVFVar(gepObject));
            if (gepVariable)
                result.insert(locationOf(gepVariable));
        }
        if (index == *upper)
            break;
    }
    return result;
}

u32_t AbstractInterpretation::getAllocaInstByteSize(const AddrStmt* address)
{
    const ICFGNode* node = address->getICFGNode();
    const auto* object = SVFUtil::dyn_cast<ObjVar>(address->getRHSVar());
    if (!object)
        throw std::invalid_argument("Addr rhs value is not ObjVar");
    const BaseObjVar* base = svfir->getBaseObject(object->getId());
    if (!base)
        return Options::MaxFieldLimit();
    if (base->isConstantByteSize())
        return base->getByteSizeOfObj();

    u64_t result = 1;
    for (const SVFVar* value : address->getArrSize())
    {
        const AD::Interval size = getInterval(value, node);
        const u64_t upper = static_cast<u64_t>(std::clamp<s64_t>(
                finiteEndpoint(size.upper(), Options::MaxFieldLimit()), 0,
                Options::MaxFieldLimit()));
        result = upper != 0 && result > Options::MaxFieldLimit() / upper
                 ? Options::MaxFieldLimit()
                 : result * upper;
    }
    return static_cast<u32_t>(result);
}

const AbstractDomain::AbstractDomain& AbstractInterpretation::
getAbstractState(const ICFGNode* node) const
{
    return state(node);
}

bool AbstractInterpretation::hasAbsState(const ICFGNode* node) const
{
    return stateTrace_.count(node) != 0;
}

AD::Location AbstractInterpretation::locationOf(const ObjVar* object) const
{
    return object ? adapter_.location(*object) : AD::Location::null();
}

const ObjVar* AbstractInterpretation::objectAt(AD::Location location) const
{
    return location.isNull() ? nullptr : &adapter_.object(location);
}

AbstractInterpretation::State AbstractInterpretation::topState()
const
{
    return State(makeNumericalDomain(false), adapter_.memoryLayout(), true);
}

AbstractInterpretation::State AbstractInterpretation::
bottomState() const
{
    return State(makeNumericalDomain(true), adapter_.memoryLayout(), true);
}

std::unique_ptr<AD::NumericalDomain>
AbstractInterpretation::makeNumericalDomain(bool bottom) const
{
    switch (Options::AEDomain())
    {
    case AENumericalDomain::Octagon:
    {
        AD::OctagonConfig config;
        config.storage = AD::OctagonStorageKind::ComponentDense;
        return std::make_unique<AD::OctagonDomain>(
                   bottom ? AD::OctagonDomain::bottom(config)
                   : AD::OctagonDomain::top(config));
    }
    case AENumericalDomain::Polyhedra:
        return std::make_unique<AD::ConvexPolyhedraDomain>(
                   bottom ? AD::ConvexPolyhedraDomain::bottom()
                   : AD::ConvexPolyhedraDomain::top());
    case AENumericalDomain::Box:
    default:
        return std::make_unique<AD::BoxDomain>(
                   bottom ? AD::BoxDomain::bottom() : AD::BoxDomain::top());
    }
}

AbstractInterpretation::State& AbstractInterpretation::
ensureState(const ICFGNode* node)
{
    auto iterator = stateTrace_.find(node);
    if (iterator == stateTrace_.end())
        iterator = stateTrace_.emplace(node, topState()).first;
    return iterator->second;
}

const AbstractInterpretation::State& AbstractInterpretation::
state(const ICFGNode* node) const
{
    const auto iterator = stateTrace_.find(node);
    if (iterator == stateTrace_.end())
        throw std::out_of_range("no dense abstract state for ICFG node");
    return iterator->second;
}

AbstractInterpretation::State& AbstractInterpretation::
scalarTransferState(const ICFGNode* node)
{
    return ensureState(node);
}

AbstractInterpretation::State AbstractInterpretation::
phiAlternativeState(const ICFGNode* predecessor)
{
    return state(predecessor);
}

void AbstractInterpretation::assignRelationalValue(
    const ValVar* target, const AD::LinearExpression& expression,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (!target || !adapter_.contains(*target))
        return;
    State& destination = ensureState(node);
    const AD::Variable variable = adapter_.variable(*target);
    destination.assignNumeric(variable, expression);
    destination.setAddressSet(variable, addresses);
}

void AbstractInterpretation::recordRelationalDependency(AD::Variable,
                                                        AD::Variable)
{
}

void AbstractInterpretation::recordRelationalSummary(AD::Variable, const State&,
                                                     const ICFGNode*)
{
}

void AbstractInterpretation::assignRelationalStore(
    const ValVar* source, AD::Variable content, const ICFGNode* node)
{
    if (!source || !adapter_.contains(*source))
        return;
    State& destination = ensureState(node);
    const AD::Variable sourceVariable = adapter_.variable(*source);
    if (!destination.numericalMayBeUninitialized(sourceVariable))
        destination.assignNumeric(content,
                                  AD::LinearExpression(sourceVariable));
}

void AbstractInterpretation::assignRelationalLoad(
    const ValVar* target, AD::Variable content, const ICFGNode* node)
{
    if (!target || !adapter_.contains(*target))
        return;
    State& destination = ensureState(node);
    if (!destination.numericalMayBeUninitialized(content))
        destination.assignNumeric(adapter_.variable(*target),
                                  AD::LinearExpression(content));
}

void AbstractInterpretation::resetAbstractState(const ICFGNode* node)
{
    stateTrace_.insert_or_assign(node, topState());
}

void AbstractInterpretation::copyAbstractState(const ICFGNode* source,
        const ICFGNode* destination)
{
    stateTrace_.insert_or_assign(destination, state(source));
}

std::unique_ptr<AbstractDomain::AbstractDomain> AbstractInterpretation::
cloneAbstractState(const ICFGNode* node) const
{
    return state(node).clone();
}

bool AbstractInterpretation::isAbstractStateEquivalent(
    const ICFGNode* node, const AbstractDomain::AbstractDomain& snapshot) const
{
    return state(node).isEquivalentTo(snapshot) ==
           AbstractDomain::CheckResult::True;
}

void AbstractInterpretation::assignInterval(State& denseState,
        AD::Variable variable,
        const AD::Interval& interval)
{
    denseState.setInterval(variable, interval);
}

void AbstractInterpretation::constrainInterval(
    State& denseState, AD::Variable variable, const AD::Interval& interval)
{
    if (interval.isBottom())
        return;

    // Constraints refine an initialized observation. Keep Top observations
    // defined even when they need no Box payload slot.
    if (denseState.interval(variable).isBottom())
        denseState.setInterval(variable, AD::Interval::top());

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

void AbstractInterpretation::assignValue(State& denseState,
        AD::Variable variable,
        const AD::Interval& interval,
        const AD::AddressSet& addresses)
{
    // SVF's synthetic extractvalue/extractelement edges can give even an
    // integer-typed SSA value an address facet. Preserve both facets through
    // assignment and a later store, just as for pointer-typed unknown values.
    assignInterval(denseState, variable, interval);
    denseState.setAddressSet(variable, addresses);
}

void AbstractInterpretation::assignMemoryValue(
    State& denseState, AD::Variable content,
    const AD::Interval& interval, const AD::AddressSet& addresses)
{
    assignInterval(denseState, content, interval);
    denseState.setAddressSet(content, addresses);
}

AD::Variable AbstractInterpretation::memoryVariable(
    const ObjVar& object, const State& denseState) const
{
    // Original's getIDFromAddr redirects freed-object accesses to its
    // BlackHole memory cell. This is an interpreter policy, not Address Top.
    if (denseState.lifetimes().mayBeFreed(adapter_.location(object)))
        return adapter_.contentVariable(*SVFUtil::cast<ObjVar>(
                                            svfir->getGNode(svfir->getBlackHoleNode())));
    return adapter_.contentVariable(object);
}

void AbstractInterpretation::materializeValue(State&, const ValVar*,
        const ICFGNode*)
{
}

void AbstractInterpretation::materializeRelations(
    State&, const std::vector<AD::Variable>&, const ICFGNode*)
{
}

void AbstractInterpretation::forgetValue(State& denseState,
        AD::Variable variable) const
{
    denseState.resetValue(variable);
}

AD::Interval AbstractInterpretation::getInterval(const ValVar* var,
        const ICFGNode* node)
{
    const AD::Interval result = getDefinedInterval(var, node);
    if (!var || !adapter_.contains(*var))
        return result;
    const State& denseState = ensureState(node);
    return denseState.numericalMayBeUninitialized(adapter_.variable(*var))
           ? AD::Interval::top() : result;
}

AD::Interval AbstractInterpretation::getDefinedInterval(const ValVar* var,
        const ICFGNode* node)
{
    AD::Interval constant;
    if (constantInterval(var, constant))
        return constant;
    // SVFIR also uses its canonical unknown pointer as the numeric source of
    // nondeterministic external-input stores (STORE_TOP annotations).
    if (var->getId() == svfir->getBlkPtr() ||
            SVFUtil::isa<BlackHoleValVar>(var))
        return AD::Interval::top();
    if (!adapter_.contains(*var))
        return AD::Interval::top();

    const State& denseState = ensureState(node);
    const AD::Variable variable = adapter_.variable(*var);
    // Global initializers can refer to a function/global before its AddrStmt.
    // This is an unresolved symbolic value, not an uninitialized memory load.
    if ((SVFUtil::isa<FunValVar>(var) || SVFUtil::isa<GlobalValVar>(var)) &&
            !denseState.hasValue(variable))
        return AD::Interval::top();
    return denseState.interval(variable);
}

AD::Interval AbstractInterpretation::getInterval(const ObjVar* var,
        const ICFGNode* node)
{
    const State& denseState = ensureState(node);
    const AD::Variable content = memoryVariable(*var, denseState);
    return denseState.interval(content);
}

AD::Interval AbstractInterpretation::getInterval(const SVFVar* var,
        const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getInterval(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getInterval(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

AD::AddressSet AbstractInterpretation::getAddressSet(const ValVar* var,
        const ICFGNode* node)
{
    if (var->getId() == IRGraph::NullPtr ||
            SVFUtil::isa<ConstNullPtrValVar>(var))
        return AD::AddressSet::singleton(AD::Location::null());
    if (var->getId() == svfir->getBlkPtr() ||
            SVFUtil::isa<BlackHoleValVar>(var))
        return blackHoleAddressSet();
    if (!adapter_.contains(*var))
        return AD::AddressSet::bottom();
    const State& denseState = ensureState(node);
    const AD::Variable variable = adapter_.variable(*var);
    return denseState.addressSet(variable);
}

AD::AddressSet AbstractInterpretation::getAddressSet(const ObjVar* var,
        const ICFGNode* node)
{
    const State& denseState = ensureState(node);
    const AD::Variable content = memoryVariable(*var, denseState);
    return denseState.addressSet(content);
}

AD::AddressSet AbstractInterpretation::getAddressSet(const SVFVar* var,
        const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getAddressSet(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getAddressSet(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

bool AbstractInterpretation::hasAbsValue(const ValVar* var,
        const ICFGNode* node) const
{
    if (SVFUtil::isa<ConstIntValVar>(var) ||
            SVFUtil::isa<ConstFPValVar>(var))
        return true;
    return stateTrace_.count(node) != 0 && adapter_.contains(*var);
}

bool AbstractInterpretation::hasAbsValue(const ObjVar* var,
        const ICFGNode* node) const
{
    const auto stateIterator = stateTrace_.find(node);
    if (stateIterator == stateTrace_.end())
        return false;
    // Defined Top is a real incoming definition even without a payload slot.
    // Inspect both guards because aggregate cells can carry either facet.
    const AD::Variable content = adapter_.contentVariable(*var);
    return stateIterator->second.hasValue(content);
}

bool AbstractInterpretation::hasAbsValue(const SVFVar* var,
        const ICFGNode* node) const
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return hasAbsValue(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return hasAbsValue(value, node);
    return false;
}

bool AbstractInterpretation::numericalValueMayBeUninitialized(
    const ValVar* var, const ICFGNode* node) const
{
    if (!var || !adapter_.contains(*var))
        return true;
    const auto stateIterator = stateTrace_.find(node);
    return stateIterator == stateTrace_.end() ||
           stateIterator->second.numericalMayBeUninitialized(
               adapter_.variable(*var));
}

void AbstractInterpretation::updateValue(const ValVar* var,
        const AD::Interval& interval,
        const AD::AddressSet& addresses,
        const ICFGNode* node)
{
    if (adapter_.contains(*var))
        assignValue(ensureState(node), adapter_.variable(*var), interval,
                    addresses);
}

void AbstractInterpretation::addUninitializedNumericalAlternative(
    const ValVar* var, const ICFGNode* node)
{
    if (var && adapter_.contains(*var))
        ensureState(node).addUninitializedNumericalAlternative(
            adapter_.variable(*var));
}

void AbstractInterpretation::updateValue(const ObjVar* var,
        const AD::Interval& interval,
        const AD::AddressSet& addresses,
        const ICFGNode* node)
{
    State& denseState = ensureState(node);
    assignMemoryValue(denseState, memoryVariable(*var, denseState), interval,
                      addresses);
}

AD::Interval AbstractInterpretation::getMemoryInterval(
    AD::Location location, const ICFGNode* node)
{
    if (location.isNull())
        return AD::Interval::bottom();
    return getInterval(&adapter_.object(location), node);
}

AD::AddressSet AbstractInterpretation::getMemoryAddressSet(
    AD::Location location, const ICFGNode* node)
{
    if (location.isNull())
        return AD::AddressSet::bottom();
    return getAddressSet(&adapter_.object(location), node);
}

bool AbstractInterpretation::hasMemoryValue(AD::Location location,
        const ICFGNode* node) const
{
    return !location.isNull() && hasAbsValue(&adapter_.object(location), node);
}

void AbstractInterpretation::updateMemoryValue(
    AD::Location location, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (!location.isNull())
        updateValue(&adapter_.object(location), interval, addresses, node);
}

void AbstractInterpretation::markFreedMemory(AD::Location location,
        const ICFGNode* node)
{
    if (!location.isNull())
        ensureState(node).lifetimes().release(location);
}

bool AbstractInterpretation::isFreedMemory(AD::Location location,
        const ICFGNode* node) const
{
    if (stateTrace_.count(node) == 0 || location.isNull())
        return false;
    return state(node).lifetimes().mayBeFreed(location);
}

void AbstractInterpretation::updateValue(const SVFVar* var,
        const AD::Interval& interval,
        const AD::AddressSet& addresses,
        const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        updateValue(object, interval, addresses, node);
    else if (const auto* scalar = SVFUtil::dyn_cast<ValVar>(var))
        updateValue(scalar, interval, addresses, node);
    else
        throw std::invalid_argument("unsupported SVF variable kind");
}


void AbstractInterpretation::loadValue(const ValVar* pointer,
                                       AD::Interval& interval,
                                       AD::AddressSet& addresses,
                                       bool& numericalMayBeUninitialized,
                                       const ICFGNode* node)
{
    numericalMayBeUninitialized = false;
    if (!adapter_.contains(*pointer))
    {
        interval = AD::Interval::bottom();
        addresses = AD::AddressSet::bottom();
        const AD::AddressSet pointees = getAddressSet(pointer, node);
        if (pointees.hasUnknownObject())
        {
            if (unknownTargetTelemetryEnabled_)
                ++unknownTargetTelemetry_.loads;
            interval = AD::Interval::top();
            addresses = AD::AddressSet::top();
            return;
        }
        for (AD::Location location : pointees)
        {
            interval.joinWith(getMemoryInterval(location, node));
            addresses.joinWith(getMemoryAddressSet(location, node));
        }
        return;
    }
    State& denseState = ensureState(node);
    materializeValue(denseState, pointer, node);
    const AD::AddressSet pointees = getAddressSet(pointer, node);
    if (pointees.hasUnknownObject())
    {
        if (unknownTargetTelemetryEnabled_)
            ++unknownTargetTelemetry_.loads;
        interval = AD::Interval::top();
        addresses = AD::AddressSet::top();
        return;
    }

    interval = AD::Interval::bottom();
    addresses = AD::AddressSet::bottom();
    for (AD::Location location : pointees.locations())
    {
        // getInterval/getAddressSet implement Original's freed-cell routing;
        // lifetime alone must not inject Top into value propagation.
        if (denseState.memoryLayout().contains(location))
        {
            const ObjVar* object = objectAt(location);
            if (!object)
                continue;
            const AD::Variable content = memoryVariable(*object, denseState);
            numericalMayBeUninitialized |=
                denseState.numericalMayBeUninitialized(content);
            interval.joinWith(denseState.interval(content));
            addresses.joinWith(denseState.addressSet(content));
        }
    }
}

void AbstractInterpretation::storeValue(const ValVar* pointer,
                                        const AD::Interval& interval,
                                        const AD::AddressSet& addresses,
                                        const ICFGNode* node)
{
    if (!adapter_.contains(*pointer))
    {
        const AD::AddressSet pointees = getAddressSet(pointer, node);
        if (pointees.hasUnknownObject() && unknownTargetTelemetryEnabled_)
            ++unknownTargetTelemetry_.stores;
        if (!pointees.hasUnknownObject())
        {
            for (AD::Location location : pointees)
                updateMemoryValue(location, interval, addresses, node);
        }
        return;
    }
    State& denseState = ensureState(node);
    materializeValue(denseState, pointer, node);
    const AD::AddressSet pointees = getAddressSet(pointer, node);
    auto write = [&](AD::Location location)
    {
        if (!denseState.memoryLayout().contains(location))
            return;
        const ObjVar* object = objectAt(location);
        if (!object)
            return;
        const AD::Variable content = memoryVariable(*object, denseState);
        // Original AE overwrites each enumerated target. This is an
        // interpreter precision policy, not the domain's weak-store semantics.
        if (!pointees.hasUnknownObject())
        {
            assignMemoryValue(denseState, content, interval, addresses);
            return;
        }
        AD::Interval joinedInterval = getInterval(object, node);
        AD::AddressSet joinedAddresses = getAddressSet(object, node);
        joinedInterval.joinWith(interval);
        joinedAddresses.joinWith(addresses);
        assignMemoryValue(denseState, content, joinedInterval,
                          joinedAddresses);
    };

    if (pointees.hasUnknownObject())
    {
        if (unknownTargetTelemetryEnabled_)
            ++unknownTargetTelemetry_.stores;
        for (const auto& [location, content] :
                denseState.memoryLayout().cells())
        {
            if (unknownTargetTelemetryEnabled_)
                ++unknownTargetTelemetry_.storeCellsVisited;
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


void AbstractInterpretation::assumeBranch(const IntraCFGEdge* edge,
        State& denseState)
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
        denseState.assume(AD::equal(
                              AD::LinearExpression(adapter_.variable(*value)),
                              AD::LinearExpression(AD::Rational(edge->getSuccessorCondValue()))));
        return;
    }

    // Preserve the established Box baseline. Relational domains additionally
    // consume the source-level comparison itself, as in a conventional
    // numerical abstract interpreter, instead of refining only its Boolean
    // SSA result. IEEE and unsigned comparisons need dedicated bit-vector/
    // NaN semantics and therefore keep the conservative legacy behavior.
    if (Options::AEDomain() != AENumericalDomain::Box &&
            !comparison->getOpVar(0)->isPointer())
    {
        const s32_t predicate = comparison->getPredicate();
        const bool supported =
            predicate == CmpStmt::ICMP_EQ ||
            predicate == CmpStmt::ICMP_NE ||
            predicate == CmpStmt::ICMP_SLT ||
            predicate == CmpStmt::ICMP_SLE ||
            predicate == CmpStmt::ICMP_SGT ||
            predicate == CmpStmt::ICMP_SGE;
        if (supported)
        {
            const ICFGNode* source = edge->getSrcNode();
            std::vector<AD::Variable> relationVariables;
            const auto expressionFor = [&](const SVFVar* operand)
                -> std::optional<AD::LinearExpression>
            {
                if (const auto* value = SVFUtil::dyn_cast<ValVar>(operand))
                {
                    if (adapter_.contains(*value))
                    {
                        materializeValue(denseState, value, source);
                        const AD::Variable variable = adapter_.variable(*value);
                        if (denseState.numericalMayBeUninitialized(variable))
                            return std::nullopt;
                        relationVariables.push_back(variable);
                        return AD::LinearExpression(variable);
                    }
                }
                const AD::Interval constant = getInterval(operand, source);
                if (constant.isSingleton())
                    return AD::LinearExpression(constant.singletonValue());
                return std::nullopt;
            };

            const auto lhs = expressionFor(comparison->getOpVar(0));
            const auto rhs = expressionFor(comparison->getOpVar(1));
            if (lhs && rhs)
            {
                materializeRelations(denseState, relationVariables, source);
                const bool taken = edge->getSuccessorCondValue() != 0;
                AD::ConstraintKind kind = AD::ConstraintKind::Equal;
                switch (predicate)
                {
                case CmpStmt::ICMP_EQ:
                    kind = taken ? AD::ConstraintKind::Equal
                           : AD::ConstraintKind::NotEqual;
                    break;
                case CmpStmt::ICMP_NE:
                    kind = taken ? AD::ConstraintKind::NotEqual
                           : AD::ConstraintKind::Equal;
                    break;
                case CmpStmt::ICMP_SLT:
                    kind = taken ? AD::ConstraintKind::LessThan
                           : AD::ConstraintKind::GreaterEqual;
                    break;
                case CmpStmt::ICMP_SLE:
                    kind = taken ? AD::ConstraintKind::LessEqual
                           : AD::ConstraintKind::GreaterThan;
                    break;
                case CmpStmt::ICMP_SGT:
                    kind = taken ? AD::ConstraintKind::GreaterThan
                           : AD::ConstraintKind::LessEqual;
                    break;
                case CmpStmt::ICMP_SGE:
                    kind = taken ? AD::ConstraintKind::GreaterEqual
                           : AD::ConstraintKind::LessThan;
                    break;
                default:
                    break;
                }
                if (std::getenv("SVF_AE_TRACE_PHI_RELATIONS"))
                    SVFUtil::outs()
                        << "AE_BRANCH_BEFORE source="
                        << edge->getSrcNode()->getId()
                        << " predicate=" << predicate
                        << " taken=" << taken << " state="
                        << denseState.numerical().toString() << '\n';
                denseState.assume(AD::LinearConstraint(*lhs - *rhs, kind));
                if (std::getenv("SVF_AE_TRACE_PHI_RELATIONS"))
                    SVFUtil::outs()
                        << "AE_BRANCH_AFTER source="
                        << edge->getSrcNode()->getId()
                        << " predicate=" << predicate
                        << " taken=" << taken << " state="
                        << denseState.numerical().toString() << '\n';
            }
        }
    }

    // Original checks only the Boolean result for feasibility. It neither
    // refines the comparison's SSA operands nor stores a branch-local Boolean
    // fact. Memory refinement is handled by collectBranchRefinement instead.
    AD::Interval result = getInterval(comparison->getRes(), edge->getSrcNode());
    if (result.isBottom())
        return;
    result.meetWith(AD::Interval::singleton(
                        AD::Rational(edge->getSuccessorCondValue())));
    if (result.isBottom())
        denseState = bottomState();
}


bool AbstractInterpretation::isBranchEdgeFeasibleAt(
    const IntraCFGEdge* edge, const ICFGNode* predecessor)
{
    State candidate = state(predecessor);
    assumeBranch(edge, candidate);
    return !candidate.isBottom();
}

} // namespace SVF
