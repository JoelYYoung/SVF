//===- AbstractStateManager.cpp -- AE domain projection helpers --------===//

#include "AE/Svfexe/AbstractInterpretation.h"

#include "SVFIR/SVFIR.h"

#include <algorithm>

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

} // namespace

const AD::AbstractDomain* AbstractInterpretation::getScalarAbstractState(
    const FunObjVar*) const
{
    return nullptr;
}

const AD::AbstractDomain* AbstractInterpretation::getScalarAbstractState(
    const ValVar*) const
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
            lower = finiteEndpoint(value.lower(), 0);
            upper = finiteEndpoint(value.upper(), Options::MaxFieldLimit());
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
                upper = finiteEndpoint(value.upper(), Options::MaxFieldLimit());
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
    if (bases.isTop())
        return AD::AddressSet::top();

    const APOffset lower = static_cast<APOffset>(std::clamp<s64_t>(
        finiteEndpoint(offset.lower(), 0), 0, Options::MaxFieldLimit()));
    const APOffset upper = static_cast<APOffset>(std::clamp<s64_t>(
        finiteEndpoint(offset.upper(), Options::MaxFieldLimit()), 0,
        Options::MaxFieldLimit()));
    AD::AddressSet result = AD::AddressSet::bottom();
    for (APOffset index = lower; index <= upper; ++index)
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
    }
    return result;
}

void AbstractInterpretation::loadValue(const ValVar* pointer,
                                       AD::Interval& interval,
                                       AD::AddressSet& addresses,
                                       const ICFGNode* node)
{
    interval = AD::Interval::bottom();
    addresses = AD::AddressSet::bottom();
    const AD::AddressSet pointees = getAddressSet(pointer, node);
    if (pointees.isTop())
    {
        interval = AD::Interval::top();
        addresses = AD::AddressSet::top();
        return;
    }
    for (AD::Location location : pointees)
    {
        interval.joinWith(getMemoryInterval(location, node));
        addresses.joinWith(getMemoryAddressSet(location, node));
    }
}

void AbstractInterpretation::storeValue(const ValVar* pointer,
                                        const AD::Interval& interval,
                                        const AD::AddressSet& addresses,
                                        const ICFGNode* node)
{
    const AD::AddressSet pointees = getAddressSet(pointer, node);
    if (pointees.isTop())
        return;
    for (AD::Location location : pointees)
        updateMemoryValue(location, interval, addresses, node);
}

const SVFType* AbstractInterpretation::getPointeeElement(const ObjVar* variable,
                                                         const ICFGNode* node)
{
    const AD::AddressSet pointees = getAddressSet(variable, node);
    if (pointees.isTop())
        return nullptr;
    for (AD::Location location : pointees)
    {
        const ObjVar* object = objectAt(location);
        if (object)
        {
            if (const BaseObjVar* base = svfir->getBaseObject(object->getId()))
                return base->getType();
        }
    }
    return nullptr;
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

} // namespace SVF
