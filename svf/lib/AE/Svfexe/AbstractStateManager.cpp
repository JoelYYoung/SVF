//===- AbstractStateManager.cpp -- AE domain projection helpers --------===//
//
//                     SVF: Static Value-Flow Analysis
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

#include "AE/Svfexe/AllocationSize.h"
#include "AE/Svfexe/AbstractInterpretation.h"

#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace SVF
{

namespace
{

s64_t finiteEndpoint(const SVF::Bound& bound, s64_t fallback)
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

SVF::Interval finiteInterval(s64_t lower, s64_t upper)
{
    return SVF::Interval::closed(SVF::Rational(lower), SVF::Rational(upper));
}

bool constantInterval(const ValVar* value, SVF::Interval& interval)
{
    if (const auto* integer = SVFUtil::dyn_cast<ConstIntValVar>(value))
    {
        interval =
            SVF::Interval::singleton(SVF::Rational(integer->getSExtValue()));
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

const SVF::AbstractDomain* AbstractInterpretation::getScalarAbstractState() const
{
    return nullptr;
}

void AbstractInterpretation::finalizeAbstractState(const ICFGNode*) {}

void AbstractInterpretation::updateInterval(const SVFVar* variable,
        const SVF::Interval& interval,
        const ICFGNode* node)
{
    updateValue(variable, interval, SVF::AddressSet::bottom(), node);
}

void AbstractInterpretation::updateAddressSet(const SVFVar* variable,
        const SVF::AddressSet& addresses,
        const ICFGNode* node)
{
    updateValue(variable, SVF::Interval::bottom(), addresses, node);
}

SVF::Interval AbstractInterpretation::getGepElementIndex(const GepStmt* gep)
{
    const ICFGNode* node = gep->getICFGNode();
    if (gep->isConstantOffset())
        return SVF::Interval::singleton(
                   SVF::Rational(static_cast<s64_t>(gep->accumulateConstantOffset())));

    SVF::Interval result = SVF::Interval::singleton(SVF::Rational());
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
            const SVF::Interval value = getInterval(variable, node);
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
        result = SVF::add(result, finiteInterval(lower, upper));
    }
    result.meetWith(
        finiteInterval(0, static_cast<s64_t>(Options::MaxFieldLimit())));
    return result.isBottom() ? SVF::Interval::singleton(SVF::Rational()) : result;
}

SVF::Interval AbstractInterpretation::getGepByteOffset(const GepStmt* gep)
{
    const ICFGNode* node = gep->getICFGNode();
    if (gep->isConstantOffset())
        return SVF::Interval::singleton(SVF::Rational(
                                            static_cast<s64_t>(gep->accumulateConstantByteOffset())));

    SVF::Interval result = SVF::Interval::singleton(SVF::Rational());
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
                const SVF::Interval value = getInterval(variable, node);
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
            result = SVF::add(result, finiteInterval(lower, upper));
        }
        else if (const auto* structure = SVFUtil::dyn_cast<SVFStructType>(type))
        {
            const s64_t offset =
                gep->getAccessPath().getStructFieldOffset(variable, structure);
            result =
                SVF::add(result, SVF::Interval::singleton(SVF::Rational(offset)));
        }
        else
        {
            throw std::invalid_argument(
                "GEP type pair must be array, pointer, or structure");
        }
    }
    return result;
}

SVF::AddressSet AbstractInterpretation::getGepObjAddrs(
    const ValVar* pointer, const SVF::Interval& offset, const ICFGNode* node)
{
    const SVF::AddressSet bases = getAddressSet(pointer, node);
    if (offset.isBottom())
        return SVF::AddressSet::bottom();

    // Empty is AE's no-target policy, not an unknown pointer. Flow-insensitive
    // points-to information must not resurrect targets killed by a later write.
    if (bases.isBottom())
        return SVF::AddressSet::bottom();

    SVF::AddressSet result = bases.hasUnknownObject()
                             ? SVF::AddressSet::objectTop()
                             : SVF::AddressSet::bottom();
    if (bases.mayContainRawAddress())
        result.joinWith(SVF::AddressSet::rawTop());
    if (bases.hasUnknownObject())
        return result;

    auto integerEndpoint = [](const SVF::Bound& bound,
                              bool lower) -> std::optional<APOffset>
    {
        if (!bound.isFinite())
        return std::nullopt;
        const SVF::Rational integer =
        lower ? (bound.isStrict() ? bound.value().floor() + SVF::Rational(1)
        : bound.value().ceil())
        : (bound.isStrict() ? bound.value().ceil() - SVF::Rational(1)
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
        return SVF::AddressSet::bottom();

    // A finite narrow range must retain its signed offsets: negative GEPs are
    // common in C++ vtable and subobject adjustment. SVFIR canonicalizes each
    // signed offset modulo the base object's field limit. For an unbounded or
    // wider range, enumerating one complete global field window covers every
    // possible canonical field without an unbounded loop.
    bool enumerateFieldUniverse = !lower || !upper;
    if (!enumerateFieldUniverse)
    {
        enumerateFieldUniverse =
            SVF::Rational(*upper) - SVF::Rational(*lower) >
            SVF::Rational(static_cast<s64_t>(Options::MaxFieldLimit()));
    }
    if (enumerateFieldUniverse)
    {
        lower = 0;
        upper = static_cast<APOffset>(Options::MaxFieldLimit());
    }

    for (APOffset index = *lower;; ++index)
    {
        for (SVF::Location base : bases)
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

AbstractInterpretation::AllocationSizeBounds
AbstractInterpretation::getAllocaInstByteSize(const AddrStmt* address)
{
    const ICFGNode* node = address->getICFGNode();
    const auto* object = SVFUtil::dyn_cast<ObjVar>(address->getRHSVar());
    if (!object)
        throw std::invalid_argument("Addr rhs value is not ObjVar");
    const BaseObjVar* base = svfir->getBaseObject(object->getId());
    if (!base)
        return {0, Options::MaxFieldLimit()};
    if (base->isConstantByteSize())
    {
        const u32_t size = base->getByteSizeOfObj();
        return {size, size};
    }

    u64_t lower = address->getArrSize().empty() ? 0 : 1;
    u64_t upper = 1;
    const auto multiply = [](u64_t product, u64_t factor)
    {
        return factor != 0 && product > Options::MaxFieldLimit() / factor
               ? Options::MaxFieldLimit() : product * factor;
    };
    for (const SVFVar* value : address->getArrSize())
    {
        const SVF::Interval size = getInterval(value, node);
        const u64_t lb = size.isBottom() ? 0 : static_cast<u64_t>(std::clamp<s64_t>(
                finiteEndpoint(size.lower(), 0), 0, Options::MaxFieldLimit()));
        const u64_t ub = size.isBottom() ? Options::MaxFieldLimit() :
                         static_cast<u64_t>(std::clamp<s64_t>(
                                                finiteEndpoint(size.upper(), Options::MaxFieldLimit()),
                                                0, Options::MaxFieldLimit()));
        lower = multiply(lower, lb);
        upper = multiply(upper, ub);
    }
    return {static_cast<u32_t>(std::min(lower, upper)), static_cast<u32_t>(upper)};
}

const AllocationSizeModel& AbstractInterpretation::allocationSizeModel()
{
    if (!allocationSizeModel_)
    {
        allocationSizeModel_ = std::make_shared<AllocationSizeModel>(*svfir);

    }
    return *allocationSizeModel_;
}

std::optional<s64_t> AbstractInterpretation::allocationByteLowerBound(
    const BaseObjVar* base)
{
    if (!base)
        return std::nullopt;
    const AllocationSize* size = allocationSizeModel().find(base);
    if (size && size->known && !size->sizeValue && size->elementBytes > 0)
        return size->constant * static_cast<s64_t>(size->elementBytes);
    if (size && size->known && size->sizeValue && size->elementBytes > 0 &&
            size->elementBytes % size->strippedCoefficient == 0)
    {
        // Every allocation at this site happens at its node, so the fixpoint
        // interval of the size value covers all of them. Use its lower
        // bound: an upper bound would accept accesses past smaller objects.
        const ICFGNode* node = size->site->getICFGNode();
        if (!hasAbsState(node))
            return std::nullopt;
        const SVF::Interval count = getInterval(size->sizeValue, node);
        if (count.isBottom())
            return std::nullopt;
        SVF::Rational lower(0);
        if (count.lower().isFinite() && count.lower().value() > lower)
            lower = count.lower().value().floor();
        const s64_t unit = size->elementBytes / size->strippedCoefficient;
        return std::stoll((lower * SVF::Rational(unit)).toString());
    }
    if (size)
        return std::nullopt;
    if (base->isConstantByteSize())
        return base->getByteSizeOfObj();
    return std::nullopt;
}

SVF::Interval AbstractInterpretation::unknownInterval(const SVFVar* var) const
{
    // The upstream Box interpreter uses mathematical intervals for unknown
    // numerical inputs. Machine-range guards belong to relational execution.
    return SVF::Interval::top();
}

void AbstractInterpretation::assignAllocationSize(const AddrStmt* addr)
{
    // Size(o) is a memory-like synthetic coordinate. The sparse executors
    // classify coordinates by object content and would treat it as a scalar,
    // so it is carried by the dense executor only (all D-005 campaigns).
    if (Options::AESparsity() != AbstractInterpretation::Dense)
        return;
    const AllocationSize* size = allocationSizeModel().find(addr);
    if (!size)
        return;
    const ICFGNode* node = addr->getICFGNode();
    State& denseState = ensureState(node);
    if (denseState.isBottom())
        return;
    const SVF::Variable target = adapter_.sizeVariable(*size->object);
    State& output = denseState;
    SVF::Interval value = SVF::Interval::top();
    std::optional<SVF::LinearExpression> expression;
    if (size->known && !size->sizeValue)
        value = SVF::Interval::singleton(SVF::Rational(size->constant));
    else if (size->known && size->sizeValue)
    {
        // Size(o) = sizeValue / strippedCoefficient (element units). It is
        // the stripped operand only if no stripped cast or multiplication
        // can wrap: the operand must be non-negative and coefficient *
        // operand must fit every integer type on the stripped chain.
        const SVF::Rational coefficient(size->strippedCoefficient);
        const SVF::Interval count = getInterval(size->sizeValue, node);
        if (!count.isBottom())
            value = SVF::Interval(
                        count.lower().isFinite()
                        ? SVF::Bound::finite((count.lower().value() / coefficient).floor())
                        : SVF::Bound::minusInfinity(),
                        count.upper().isFinite()
                        ? SVF::Bound::finite((count.upper().value() / coefficient).ceil())
                        : SVF::Bound::plusInfinity());
        if (const auto* operand = size->operand)
        {
            s64_t stripped = 1;
            std::vector<const SVFVar*> chain;
            AllocationSizeModel::stripCoefficient(size->sizeValue, stripped, &chain);
            // A machine value lies in its own type's range even when the
            // mathematical interval has not been clipped to it.
            SVF::Interval source = getDefinedInterval(operand, node);
            if (!source.isBottom() && operand->getType())
                source.meetWith(utils->getRangeLimitFromType(operand->getType()));
            bool exact = adapter_.contains(*operand) && !source.isBottom() &&
                         source.lower().isFinite() &&
                         source.lower().value() >= SVF::Rational(0);
            const SVF::Interval scaled = exact
                                         ? SVF::multiply(source, SVF::Interval::singleton(coefficient))
                                         : SVF::Interval::top();
            for (const SVFVar* step : chain)
                exact = exact && step->getType() &&
                        scaled.isSubsetOf(utils->getRangeLimitFromType(step->getType()));

            if (exact)
                expression = SVF::LinearExpression(adapter_.variable(*operand));
        }
    }

    if (expression)
        output.assignNumeric(target, *expression);
    else
        assignInterval(output, target, value);

}

const SVF::AbstractDomain& AbstractInterpretation::
getAbstractState(const ICFGNode* node) const
{
    return state(node);
}

bool AbstractInterpretation::hasAbsState(const ICFGNode* node) const
{
    return stateTrace_.count(node) != 0;
}

SVF::Location AbstractInterpretation::locationOf(const ObjVar* object) const
{
    return object ? adapter_.location(*object) : SVF::Location::null();
}

const ObjVar* AbstractInterpretation::objectAt(SVF::Location location) const
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

std::unique_ptr<SVF::NumericalDomain>
AbstractInterpretation::makeNumericalDomain(bool bottom) const
{
    return std::make_unique<SVF::BoxDomain>(bottom ? SVF::BoxDomain::bottom() : SVF::BoxDomain::top());
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

void AbstractInterpretation::resetAbstractState(const ICFGNode* node)
{
    stateTrace_.insert_or_assign(node, topState());
}

void AbstractInterpretation::copyAbstractState(const ICFGNode* source,
        const ICFGNode* destination)
{
    stateTrace_.insert_or_assign(destination, state(source));
}

std::unique_ptr<SVF::AbstractDomain> AbstractInterpretation::
cloneAbstractState(const ICFGNode* node) const
{
    return state(node).clone();
}

bool AbstractInterpretation::isAbstractStateEquivalent(
    const ICFGNode* node, const SVF::AbstractDomain& snapshot) const
{
    return state(node).isEquivalentTo(snapshot) ==
           SVF::CheckResult::True;
}

void AbstractInterpretation::assignInterval(State& denseState,
        SVF::Variable variable,
        const SVF::Interval& interval)
{
    denseState.setInterval(variable, interval);
}

void AbstractInterpretation::constrainInterval(
    State& denseState, SVF::Variable variable,
    const SVF::Interval& interval) const
{
    if (interval.isBottom())
        return;

    // Constraints refine an initialized observation. Keep Top observations
    // defined even when they need no Box payload slot.
    if (denseState.interval(variable).isBottom())
        denseState.setInterval(variable, SVF::Interval::top());

    SVF::LinearConstraintSet constraints;
    SVF::LinearExpression expression(variable);
    if (interval.lower().isFinite())
    {
        constraints.emplace_back(
            expression - SVF::LinearExpression(interval.lower().value()),
            interval.lower().isStrict() ? SVF::ConstraintKind::GreaterThan
            : SVF::ConstraintKind::GreaterEqual);
    }
    if (interval.upper().isFinite())
    {
        constraints.emplace_back(
            expression - SVF::LinearExpression(interval.upper().value()),
            interval.upper().isStrict() ? SVF::ConstraintKind::LessThan
            : SVF::ConstraintKind::LessEqual);
    }
    denseState.numerical().assumeAll(constraints);
}

void AbstractInterpretation::assignValue(State& denseState,
        SVF::Variable variable,
        const SVF::Interval& interval,
        const SVF::AddressSet& addresses)
{
    // SVF's synthetic extractvalue/extractelement edges can give even an
    // integer-typed SSA value an address facet. Preserve both facets through
    // assignment and a later store, just as for pointer-typed unknown values.
    assignInterval(denseState, variable, interval);
    denseState.setAddressSet(variable, addresses);
}

void AbstractInterpretation::assignMemoryValue(
    State& denseState, SVF::Variable content,
    const SVF::Interval& interval, const SVF::AddressSet& addresses)
{
    assignInterval(denseState, content, interval);
    denseState.setAddressSet(content, addresses);
}

SVF::Variable AbstractInterpretation::memoryVariable(
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
    State&, const std::vector<SVF::Variable>&, const ICFGNode*)
{
}

void AbstractInterpretation::forgetValue(State& denseState,
        SVF::Variable variable) const
{
    denseState.resetValue(variable);
}

SVF::Interval AbstractInterpretation::getInterval(const ValVar* var,
        const ICFGNode* node)
{
    const SVF::Interval result = getDefinedInterval(var, node);
    // Upstream reads the stored interval even when an uninitialized
    // alternative is present; initialization remains a separate detector fact.
    return result;
}

SVF::Interval AbstractInterpretation::getDefinedInterval(const ValVar* var,
        const ICFGNode* node)
{
    SVF::Interval constant;
    if (constantInterval(var, constant))
        return constant;
    // SVFIR also uses its canonical unknown pointer as the numeric source of
    // nondeterministic external-input stores (STORE_TOP annotations).
    if (var->getId() == svfir->getBlkPtr() ||
            SVFUtil::isa<BlackHoleValVar>(var))
        return SVF::Interval::top();
    if (!adapter_.contains(*var))
        return SVF::Interval::top();

    const State& denseState = ensureState(node);
    const SVF::Variable variable = adapter_.variable(*var);
    // Global initializers can refer to a function/global before its AddrStmt.
    // This is an unresolved symbolic value, not an uninitialized memory load.
    if ((SVFUtil::isa<FunValVar>(var) || SVFUtil::isa<GlobalValVar>(var)) &&
            !denseState.hasValue(variable))
        return SVF::Interval::top();
    return denseState.interval(variable);
}

SVF::Interval AbstractInterpretation::getInterval(const ObjVar* var,
        const ICFGNode* node)
{
    const State& denseState = ensureState(node);
    const SVF::Variable content = memoryVariable(*var, denseState);
    return denseState.interval(content);
}

SVF::Interval AbstractInterpretation::getInterval(const SVFVar* var,
        const ICFGNode* node)
{
    if (const auto* object = SVFUtil::dyn_cast<ObjVar>(var))
        return getInterval(object, node);
    if (const auto* value = SVFUtil::dyn_cast<ValVar>(var))
        return getInterval(value, node);
    throw std::invalid_argument("unsupported SVF variable kind");
}

SVF::AddressSet AbstractInterpretation::getAddressSet(const ValVar* var,
        const ICFGNode* node)
{
    if (var->getId() == IRGraph::NullPtr ||
            SVFUtil::isa<ConstNullPtrValVar>(var))
        return SVF::AddressSet::singleton(SVF::Location::null());
    if (var->getId() == svfir->getBlkPtr() ||
            SVFUtil::isa<BlackHoleValVar>(var))
        return blackHoleAddressSet();
    if (!adapter_.contains(*var))
        return SVF::AddressSet::bottom();
    const State& denseState = ensureState(node);
    const SVF::Variable variable = adapter_.variable(*var);
    return denseState.addressSet(variable);
}

SVF::AddressSet AbstractInterpretation::getAddressSet(const ObjVar* var,
        const ICFGNode* node)
{
    const State& denseState = ensureState(node);
    const SVF::Variable content = memoryVariable(*var, denseState);
    return denseState.addressSet(content);
}

SVF::AddressSet AbstractInterpretation::getAddressSet(const SVFVar* var,
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
    const SVF::Variable content = adapter_.contentVariable(*var);
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
        const SVF::Interval& interval,
        const SVF::AddressSet& addresses,
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
        const SVF::Interval& interval,
        const SVF::AddressSet& addresses,
        const ICFGNode* node)
{
    State& denseState = ensureState(node);
    assignMemoryValue(denseState, memoryVariable(*var, denseState), interval,
                      addresses);
}

SVF::Interval AbstractInterpretation::getMemoryInterval(
    SVF::Location location, const ICFGNode* node)
{
    if (location.isNull())
        return SVF::Interval::bottom();
    return getInterval(&adapter_.object(location), node);
}

SVF::AddressSet AbstractInterpretation::getMemoryAddressSet(
    SVF::Location location, const ICFGNode* node)
{
    if (location.isNull())
        return SVF::AddressSet::bottom();
    return getAddressSet(&adapter_.object(location), node);
}

bool AbstractInterpretation::hasMemoryValue(SVF::Location location,
        const ICFGNode* node) const
{
    return !location.isNull() && hasAbsValue(&adapter_.object(location), node);
}

void AbstractInterpretation::updateMemoryValue(
    SVF::Location location, const SVF::Interval& interval,
    const SVF::AddressSet& addresses, const ICFGNode* node)
{
    if (!location.isNull())
        updateValue(&adapter_.object(location), interval, addresses, node);
}

void AbstractInterpretation::markFreedMemory(SVF::Location location,
        const ICFGNode* node)
{
    if (!location.isNull())
        ensureState(node).lifetimes().release(location);
}

bool AbstractInterpretation::isFreedMemory(SVF::Location location,
        const ICFGNode* node) const
{
    if (stateTrace_.count(node) == 0 || location.isNull())
        return false;
    return state(node).lifetimes().mayBeFreed(location);
}

void AbstractInterpretation::updateValue(const SVFVar* var,
        const SVF::Interval& interval,
        const SVF::AddressSet& addresses,
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
                                       SVF::Interval& interval,
                                       SVF::AddressSet& addresses,
                                       bool& numericalMayBeUninitialized,
                                       const ICFGNode* node)
{
    numericalMayBeUninitialized = false;
    if (!adapter_.contains(*pointer))
    {
        interval = SVF::Interval::bottom();
        addresses = SVF::AddressSet::bottom();
        const SVF::AddressSet pointees = getAddressSet(pointer, node);
        if (pointees.hasUnknownObject())
        {

            interval = SVF::Interval::top();
            addresses = SVF::AddressSet::top();
            return;
        }
        for (SVF::Location location : pointees)
        {
            interval.joinWith(getMemoryInterval(location, node));
            addresses.joinWith(getMemoryAddressSet(location, node));
        }
        return;
    }
    State& denseState = ensureState(node);
    materializeValue(denseState, pointer, node);
    const SVF::AddressSet pointees = getAddressSet(pointer, node);
    if (pointees.hasUnknownObject())
    {

        interval = SVF::Interval::top();
        addresses = SVF::AddressSet::top();
        return;
    }

    interval = SVF::Interval::bottom();
    addresses = SVF::AddressSet::bottom();
    for (SVF::Location location : pointees.locations())
    {
        // getInterval/getAddressSet implement Original's freed-cell routing;
        // lifetime alone must not inject Top into value propagation.
        if (denseState.memoryLayout().contains(location))
        {
            const ObjVar* object = objectAt(location);
            if (!object)
                continue;
            const SVF::Variable content = memoryVariable(*object, denseState);
            numericalMayBeUninitialized |=
                denseState.numericalMayBeUninitialized(content);
            interval.joinWith(denseState.interval(content));
            addresses.joinWith(denseState.addressSet(content));
        }
    }
}

bool AbstractInterpretation::canStrongStore(
    const SVF::AddressSet& pointees, const ObjVar& object, const State& state)
{
    const BaseObjVar* base = svfir->getBaseObject(object.getId());
    {
        const NodeID id = object.getId();
        const auto* pta = preAnalysis->getPointerAnalysis();
        return pointees.isSingleton() && base && !svfir->isNullOrBlkObj(id) &&
               !pta->isHeapMemObj(id) && !base->isFieldInsensitive() &&
               !pta->isLocalVarInRecursiveFun(id) &&
               (!pta->isArrayMemObj(id) || svfir->isGepObjVar(id)) &&
               base->getType()->getTypeInfo()->getNumOfFlattenElements() <=
               base->getMaxFieldOffsetLimit() &&
               !state.lifetimes().mayBeFreed(adapter_.location(object));
    }
    return pointees.isSingleton() && base &&
           !base->isHeap() && !base->isArray() &&
           !base->isBlackHoleObj() &&
           !base->isFieldInsensitive() &&
           !(base->isStack() && isRecursiveFun(base->getFunction())) &&
           !adapter_.isRuntimeFieldSummary(object) &&
           !state.lifetimes().mayBeFreed(adapter_.location(object));
}

bool AbstractInterpretation::canRefineMemory(
    const SVF::AddressSet& pointees, const ObjVar& object, const State& state)
{
    const BaseObjVar* base = svfir->getBaseObject(object.getId());
    // One abstract allocation site can denote several concrete heap objects.
    // Strong updates require a non-repeating site and a unique, non-repeating
    // caller chain; a singleton points-to set alone is insufficient.
    if (base && base->isHeap())
    {

        const ICFGNode* site = base->getICFGNode();
        std::set<const FunObjVar*> visited;
        while (site)
        {
            const FunObjVar* function = site->getFun();
            if (!function || !visited.insert(function).second || isRecursiveFun(function))
                return false;
            const auto& wtos = preAnalysis->getFuncToWTO();
            const auto found = wtos.find(function);
            if (found == wtos.end() || !found->second->in_cycleDepth_table(site) ||
                    found->second->isHead(site) ||
                    found->second->cycleDepth(site).begin() != found->second->cycleDepth(site).end())
                return false;
            std::set<const CallICFGNode*> callers;
            for (const auto* edge : callGraph->getCallGraphNode(function)->getInEdges())
            {
                callers.insert(edge->getDirectCalls().begin(), edge->getDirectCalls().end());
                callers.insert(edge->getIndirectCalls().begin(), edge->getIndirectCalls().end());
            }
            if (callers.size() > 1)
                return false;
            if (callers.empty())
                break;
            site = *callers.begin();
        }
        if (!site)
            return false;
    }
    return pointees.isSingleton() && base &&
           !base->isBlackHoleObj() &&
           !base->isFieldInsensitive() &&
           !adapter_.isRuntimeFieldSummary(object) &&
           !state.lifetimes().mayBeFreed(adapter_.location(object));
}

void AbstractInterpretation::storeValue(const ValVar* pointer,
                                        const SVF::Interval& interval,
                                        const SVF::AddressSet& addresses,
                                        const ICFGNode* node)
{
    State& denseState = ensureState(node);
    if (adapter_.contains(*pointer))
        materializeValue(denseState, pointer, node);
    const SVF::AddressSet pointees = getAddressSet(pointer, node);
    auto write = [&](SVF::Location location)
    {
        if (!denseState.memoryLayout().contains(location))
            return;
        const ObjVar* object = objectAt(location);
        if (!object)
            return;
        const SVF::Variable content = memoryVariable(*object, denseState);
        if (canStrongStore(pointees, *object, denseState))
        {
            assignMemoryValue(denseState, content, interval, addresses);
            return;
        }
        // This cell may not be written (or may summarize several concrete
        // cells). Retain the unchanged alternative, including initialization
        // and address facets. Sequential weak writes overapproximate the
        // choice of one target; they do not assert that all targets changed.
        {
            State written = topState();
            assignMemoryValue(written, content, interval, addresses);
            denseState.joinValueFrom(content, written, content);
        }
    };

    if (pointees.hasUnknownObject())
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
        for (SVF::Location location : pointees.locations())
            write(location);
    }
}

std::optional<SVF::LinearExpression>
AbstractInterpretation::reconstructLinearExpression(
    const SVFVar* operand, State& denseState, const ICFGNode* node,
    std::vector<SVF::Variable>& relationVariables, unsigned depth,
    std::set<NodeID>& visiting)
{
    const auto* value = SVFUtil::dyn_cast<ValVar>(operand);
    if (!value)
    {
        const SVF::Interval constant = getInterval(operand, node);
        return constant.isSingleton()
               ? std::optional<SVF::LinearExpression>(
                   SVF::LinearExpression(constant.singletonValue()))
               : std::nullopt;
    }

    SVF::Interval literal;
    if (constantInterval(value, literal) && literal.isSingleton())
        return SVF::LinearExpression(literal.singletonValue());
    if (!adapter_.contains(*value))
        return std::nullopt;

    materializeValue(denseState, value, node);
    const SVF::Variable variable = adapter_.variable(*value);
    if (denseState.numericalMayBeUninitialized(variable))
        return std::nullopt;
    const SVF::Interval valueInterval = denseState.interval(variable);
    if (valueInterval.isSingleton())
        return SVF::LinearExpression(valueInterval.singletonValue());

    const auto retainValue = [&]() -> std::optional<SVF::LinearExpression>
    {
        relationVariables.push_back(variable);
        return SVF::LinearExpression(variable);
    };
    if (depth == 0 || value->getInEdges().size() != 1 ||
            !visiting.insert(value->getId()).second)
        return retainValue();

    const auto* binary = SVFUtil::dyn_cast<BinaryOPStmt>(
                             *value->getInEdges().begin());
    const bool supported = binary &&
                           (binary->getOpcode() == BinaryOPStmt::Add ||
                            binary->getOpcode() == BinaryOPStmt::Sub ||
                            binary->getOpcode() == BinaryOPStmt::Mul);
    const auto* integerType = binary
                              ? SVFUtil::dyn_cast<SVFIntegerType>(
                                  binary->getRes()->getType())
                              : nullptr;
    if (!supported || !integerType)
    {
        visiting.erase(value->getId());
        return retainValue();
    }

    const auto intervalFor = [&](const SVFVar* input) -> SVF::Interval
    {
        if (const auto* inputValue = SVFUtil::dyn_cast<ValVar>(input))
        {
            SVF::Interval constant;
            if (constantInterval(inputValue, constant))
                return constant;
            if (adapter_.contains(*inputValue))
            {
                materializeValue(denseState, inputValue, node);
                const SVF::Variable inputVariable =
                adapter_.variable(*inputValue);
                if (denseState.numericalMayBeUninitialized(inputVariable))
                    return SVF::Interval::bottom();
                return denseState.interval(inputVariable);
            }
        }
        return getInterval(input, node);
    };

    const SVF::Interval lhsInterval = intervalFor(binary->getOpVar(0));
    const SVF::Interval rhsInterval = intervalFor(binary->getOpVar(1));
    SVF::Interval mathematical = SVF::Interval::top();
    if (!lhsInterval.isBottom() && !rhsInterval.isBottom())
    {
        if (binary->getOpcode() == BinaryOPStmt::Add)
            mathematical = SVF::add(lhsInterval, rhsInterval);
        else if (binary->getOpcode() == BinaryOPStmt::Sub)
            mathematical = SVF::subtract(lhsInterval, rhsInterval);
        else
            mathematical = SVF::multiply(lhsInterval, rhsInterval);
    }
    const SVF::Interval typeRange =
        utils->getRangeLimitFromType(binary->getRes()->getType());
    const bool noIntegerWrap = !mathematical.isBottom() &&
                               mathematical.isSubsetOf(typeRange);
    if (!noIntegerWrap)
    {
        visiting.erase(value->getId());
        return retainValue();
    }

    std::vector<SVF::Variable> expandedVariables;
    const auto lhs = reconstructLinearExpression(
                         binary->getOpVar(0), denseState, node,
                         expandedVariables, depth - 1, visiting);
    const auto rhs = reconstructLinearExpression(
                         binary->getOpVar(1), denseState, node,
                         expandedVariables, depth - 1, visiting);
    visiting.erase(value->getId());
    if (!lhs || !rhs)
        return retainValue();

    std::optional<SVF::LinearExpression> result;
    if (binary->getOpcode() == BinaryOPStmt::Add)
        result = *lhs + *rhs;
    else if (binary->getOpcode() == BinaryOPStmt::Sub)
        result = *lhs - *rhs;
    else if (lhsInterval.isSingleton())
        result = lhsInterval.singletonValue() * *rhs;
    else if (rhsInterval.isSingleton())
        result = rhsInterval.singletonValue() * *lhs;

    // Bound expansion even for Polyhedra. Octagon queries normally stop at
    // two variables, while this small slack lets constants and copies fold.
    if (!result || result->terms().size() > 8)
        return retainValue();
    relationVariables.insert(relationVariables.end(),
                             expandedVariables.begin(),
                             expandedVariables.end());
    return result;
}

void AbstractInterpretation::assumeBranch(const IntraCFGEdge* edge,
        State& denseState)
{
    const SVFVar* condition = edge->getCondition();
    if (!condition)
        return;
    if (edge->isSwitchEdge())
    {
        // A default destination can also contain explicit labels. Its union
        // cannot be approximated by the hull of those labels alone.
        const auto& cases = edge->getSwitchCases();
        // Narrow integer values may retain a zero-extended representation in
        // the abstract state. Do not intersect them with signed IR labels.
        if (edge->getSwitchWidth() < 32 || edge->getSwitchWidth() > 64)
            return;
        if (edge->includesSwitchDefault())
        {
            // A singleton matching an explicit label cannot take default,
            // unless that label shares this destination. Never interpret the
            // default sentinel as a numerical case value.
            const SVF::Interval value = getInterval(condition, edge->getSrcNode());
            if (value.isSingleton())
                for (const auto* outgoing : edge->getSrcNode()->getOutEdges())
                    if (const auto* branch = SVFUtil::dyn_cast<IntraCFGEdge>(outgoing))
                        if (branch->isSwitchEdge() && branch != edge)
                            for (s64_t label : branch->getSwitchCases())
                                if (value.singletonValue() == SVF::Rational(label))
                                    denseState = bottomState();
            return;
        }
        if (cases.empty())
            return;
        const auto* value = SVFUtil::dyn_cast<ValVar>(condition);
        if (!value || !adapter_.contains(*value)) return;
        const auto limits = std::minmax_element(cases.begin(), cases.end());
        materializeValue(denseState, value, edge->getSrcNode());
        const SVF::LinearExpression expression(adapter_.variable(*value));
        denseState.assume(SVF::LinearConstraint(expression - SVF::LinearExpression(SVF::Rational(*limits.first)),
                                                SVF::ConstraintKind::GreaterEqual));
        denseState.assume(SVF::LinearConstraint(expression - SVF::LinearExpression(SVF::Rational(*limits.second)),
                                                SVF::ConstraintKind::LessEqual));
        return;
    }
    if (condition->getInEdges().empty())
        return;
    const auto* comparison =
        SVFUtil::dyn_cast<CmpStmt>(*condition->getInEdges().begin());
    if (!comparison)
    {
        const auto* value = SVFUtil::dyn_cast<ValVar>(condition);
        if (!value || !adapter_.contains(*value))
            return;
        materializeValue(denseState, value, edge->getSrcNode());
        denseState.assume(SVF::equal(
                              SVF::LinearExpression(adapter_.variable(*value)),
                              SVF::LinearExpression(SVF::Rational(edge->getSuccessorCondValue()))));
        return;
    }

    // Preserve the established Box baseline. Relational domains additionally
    // consume the source-level comparison itself, as in a conventional
    // numerical abstract interpreter, instead of refining only its Boolean
    // SSA result. IEEE and unsigned comparisons need dedicated bit-vector/
    // NaN semantics and therefore keep the conservative legacy behavior.

    // Original checks only the Boolean result for feasibility. It neither
    // refines the comparison's SSA operands nor stores a branch-local Boolean
    // fact. Memory refinement is handled by collectBranchRefinement instead.
    SVF::Interval result = getInterval(comparison->getRes(), edge->getSrcNode());
    if (result.isBottom())
        return;
    result.meetWith(SVF::Interval::singleton(
                        SVF::Rational(edge->getSuccessorCondValue())));
    if (result.isBottom())
        denseState = bottomState();
}

bool AbstractInterpretation::isBranchEdgeFeasibleAt(
    const IntraCFGEdge* edge, const ICFGNode* predecessor)
{
    if (!edge->isSwitchEdge())
    {
        const SVFVar* condition = edge->getCondition();
        const auto* comparison = condition && !condition->getInEdges().empty()
                                 ? SVFUtil::dyn_cast<CmpStmt>(
                                     *condition->getInEdges().begin())
                                 : nullptr;
        if (comparison)
        {
            if (state(predecessor).isBottom())
                return false;
            SVF::Interval result = getInterval(comparison->getRes(),
                                               edge->getSrcNode());
            if (result.isBottom())
                return true;
            result.meetWith(SVF::Interval::singleton(
                                SVF::Rational(edge->getSuccessorCondValue())));
            return !result.isBottom();
        }
    }
    State candidate = state(predecessor);
    assumeBranch(edge, candidate);
    return !candidate.isBottom();
}

} // namespace SVF
