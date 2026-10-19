//===- AEDetector.cpp -- Vulnerability
// Detectors---------------------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

//
//  Created on: May 1, 2025
//      Author: Xiao Cheng, Jiawei Wang, Mingxiu Wang
//

#include <AE/Svfexe/AEDetector.h>
#include <AE/Svfexe/AbsExtAPI.h>
#include "AE/Svfexe/AllocationSize.h"
#include <limits>
#include <AE/Svfexe/AbstractInterpretation.h>

using namespace SVF;

namespace
{
SVF::Interval integerInterval(s64_t value)
{
    return SVF::Interval::singleton(SVF::Rational(value));
}

bool upperAtLeast(const SVF::Interval& interval, u32_t size)
{
    return !interval.upper().isFinite() ||
           interval.upper().value() >= SVF::Rational(size);
}

SVF::Interval detectorGepOffset(const GepStmt* gep)
{
    // Address-carrier field limits must never truncate a detector bound.
    if (gep->isConstantOffset())
        return integerInterval(gep->accumulateConstantByteOffset());
    auto& ae = AbstractInterpretation::getAEInstance();
    SVF::Interval result = integerInterval(0);
    for (const auto& part : gep->getOffsetVarAndGepTypePairVec())
    {
        const auto* variable = part.first;
        const auto* type = part.second;
        if (const auto* structure = SVFUtil::dyn_cast<SVFStructType>(type))
            result = SVF::add(result, integerInterval(gep->getAccessPath().getStructFieldOffset(variable, structure)));
        else if (SVFUtil::isa<SVFArrayType>(type) || SVFUtil::isa<SVFPointerType>(type))
        {
            const auto* array = SVFUtil::dyn_cast<SVFArrayType>(type);
            const auto size = array ? array->getTypeOfElement()->getByteSize()
                              : gep->getAccessPath().gepSrcPointeeType()->getByteSize();
            const auto* constant = SVFUtil::dyn_cast<ConstIntValVar>(variable);
            auto index = constant ? integerInterval(constant->getSExtValue())
                         : ae.getInterval(variable, gep->getICFGNode());
            result = SVF::add(result, SVF::multiply(index, integerInterval(size)));
        }
        else return SVF::Interval::top();
    }
    return result;
}

/// Byte offset of a GEP as constant + scale * index with at most one
/// variable index; false when the offset has another shape.
bool decomposeGepOffset(const GepStmt* gep, s64_t& constant,
                        const ValVar*& index, s64_t& scale)
{
    constant = 0;
    index = nullptr;
    scale = 0;
    if (gep->isConstantOffset())
    {
        constant = gep->accumulateConstantByteOffset();
        return true;
    }
    for (const auto& part : gep->getOffsetVarAndGepTypePairVec())
    {
        const auto* variable = part.first;
        const auto* type = part.second;
        if (const auto* structure = SVFUtil::dyn_cast<SVFStructType>(type))
            constant += gep->getAccessPath().getStructFieldOffset(variable, structure);
        else if (SVFUtil::isa<SVFArrayType>(type) || SVFUtil::isa<SVFPointerType>(type))
        {
            const auto* array = SVFUtil::dyn_cast<SVFArrayType>(type);
            const s64_t size = array ? array->getTypeOfElement()->getByteSize()
                               : gep->getAccessPath().gepSrcPointeeType()->getByteSize();
            if (const auto* literal = SVFUtil::dyn_cast<ConstIntValVar>(variable))
                constant += literal->getSExtValue() * size;
            else if (!index && SVFUtil::isa<ValVar>(variable))
            {
                index = SVFUtil::cast<ValVar>(variable);
                scale = size;
            }
            else
                return false;
        }
        else
            return false;
    }
    return true;
}

std::vector<u32_t> nullDerefArgumentIndices(const CallICFGNode* call)
{
    std::vector<u32_t> result;
    for (const std::string& annotation :
            ExtAPI::getExtAPI()->getExtFuncAnnotations(call->getCalledFunction()))
    {
        if (annotation.find("MEMCPY") != std::string::npos)
        {
            // MEMCPY denotes memory effects, not an argument-count ABI.
            // memcpy/memmove intrinsics, fortified copies, memccpy, bcopy
            // and strncpy access arguments 0/1. The trailing length,
            // volatility, sentinel and destination-size values are scalars.
            // iconv is the registered exception: its descriptor is opaque;
            // the four following arguments describe the buffers and sizes.
            const std::vector<u32_t> accessed =
                call->getCalledFunction()->getName() == "iconv"
                ? std::vector<u32_t> {1, 2, 3, 4}
                :
                std::vector<u32_t> {0, 1};
            for (u32_t index : accessed)
            {
                if (index >= call->arg_size() ||
                        !call->getArgument(index)->isPointer())
                    throw std::invalid_argument(
                        "MEMCPY detector memory role does not match pointer argument type");
                result.push_back(index);
            }
        }
        else if (annotation.find("MEMSET") != std::string::npos)
            result.push_back(0);
        else if (annotation.find("STRCPY") != std::string::npos ||
                 annotation.find("STRCAT") != std::string::npos)
        {
            result.push_back(0);
            result.push_back(1);
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
} // namespace
/**
 * @brief Detects buffer overflow issues within a given ICFG node.
 *
 * This function handles both non-call nodes, where it analyzes GEP
 * (GetElementPtr) instructions for potential buffer overflows, and call nodes,
 * where it checks for external API calls that may cause overflows.
 *
 * @param as Reference to the abstract state.
 * @param node Pointer to the ICFG node.
 */
void BufOverflowDetector::detect(const ICFGNode* node)
{
    auto& ae = AbstractInterpretation::getAEInstance();
    if (!SVFUtil::isa<CallICFGNode>(node))
    {
        // Handle non-call nodes by analyzing GEP instructions
        for (const SVFStmt* stmt : node->getSVFStmts())
        {
            if (const GepStmt* gep = SVFUtil::dyn_cast<GepStmt>(stmt))
            {
                SVFIR* svfir = PAG::getPAG();
                bool unsupported = false;
                bool mayOverflow = false;
                bool checkedTarget = false;
                bool relational = false;

                // Update the GEP object offset from its base
                const SVF::AddressSet lhsVal =
                    ae.getAddressSet(gep->getLHSVar(), node);
                const SVF::AddressSet rhsVal =
                    ae.getAddressSet(gep->getRHSVar(), node);
                updateGepObjOffsetFromBase(node, lhsVal, rhsVal,
                                           detectorGepOffset(gep));

                if (rhsVal.isBottom() || !rhsVal.isFinite() ||
                        rhsVal.hasUnknownObject())
                    unsupported = true;
                else for (SVF::Location location : rhsVal)
                    {
                        const ObjVar* object = ae.objectAt(location);
                        if (!object)
                        {
                            unsupported = true;
                            continue;
                        }
                        NodeID objId = object->getId();
                        const BaseObjVar* baseObject = svfir->getBaseObject(objId);
                        if (!baseObject)
                        {
                            unsupported = true;
                            continue;
                        }
                        // D-005: a sound lower bound on every allocation at the
                        // site (the previous code used the dynamic size's upper
                        // bound and ignored the alloca element size).
                        const auto lower = ae.allocationByteLowerBound(baseObject);
                        if (!lower)
                        {
                            unsupported = true;
                            continue;
                        }
                        checkedTarget = true;

                        // Calculate access offset and check for potential overflow
                        SVF::Interval accessOffset = getAccessOffset(objId, gep);
                        const bool lowerUnsafe = !accessOffset.lower().isFinite() ||
                                                 accessOffset.lower().value() < SVF::Rational(0);
                        if (!lowerUnsafe && accessOffset.upper().isFinite() &&
                                accessOffset.upper().value() <= SVF::Rational(*lower))
                            continue;
                        if (!lowerUnsafe && relationalUpperBound(node, object, gep, 0))
                        {
                            relational = true;
                            continue;
                        }
                        const AllocationSize* model = ae.allocationSizeModel().find(baseObject);
                        if (*lower == 0 && !(model && model->known && model->sizeValue))
                        {
                            // Static zero or unsized extents (e.g. extern T[0])
                            // give no usable range; a dynamic size whose lower
                            // bound is 0 falls through to May.
                            unsupported = true;
                            continue;
                        }
                        if (std::getenv("SVF_AE_TRACE_SIZE"))
                            std::cerr << "AE_SIZE_GEP_MAY recheck=" << (ae.recheckStates_ != nullptr) << " node=" << node->getId()
                                      << " object=" << objId << " offset=" << accessOffset.toString()
                                      << " lower_bytes=" << *lower;
                        if (std::getenv("SVF_AE_TRACE_SIZE"))
                            for (const auto& part : gep->getOffsetVarAndGepTypePairVec())
                                std::cerr << " part=" << part.first->getId() << ":"
                                          << ae.getInterval(part.first, node).toString();
                        if (std::getenv("SVF_AE_TRACE_SIZE"))
                            std::cerr << '\n';
                        mayOverflow = true;
                        // An out-of-range address-formation property is not
                        // itself a memory-access bug (non-inbounds GEP).
                    }
                const auto outcome = mayOverflow
                                     ? AbstractInterpretation::QueryOutcome::May
                                     : (unsupported || !checkedTarget
                                        ? AbstractInterpretation::QueryOutcome::Unsupported
                                        : AbstractInterpretation::QueryOutcome::Safe);
                ae.recordQuery(BUF_OVERFLOW, node, gep->getRHSVar(),
                               "gep-allocation-range", outcome,
                               mayOverflow ? "formed offset may be outside [0, allocation size]; not an access verdict"
                               : (unsupported || !checkedTarget
                                  ? "object or object size is unsupported"
                                  : relational
                                  ? "formed offsets are within [0, allocation size], including one-past; not a dereference verdict [relational-size]"
                                  : "formed offsets are within [0, allocation size], including one-past; not a dereference verdict"), gep);
            }
            else if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(stmt))
                detectMemoryBounds(load, load->getRHSVar(), "load-bounds");
            else if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(stmt))
            {
                // Global initializer StoreStmts are synthetic writes, not
                // LLVM memory accesses. Only instruction sites have a width.
                if (node->getFun())
                    detectMemoryBounds(store, store->getLHSVar(), "store-bounds");
            }
        }
    }
    else
    {
        // Handle call nodes by checking for external API calls
        const CallICFGNode* callNode = SVFUtil::cast<CallICFGNode>(node);
        if (SVFUtil::isExtCall(callNode->getCalledFunction()))
        {
            detectExtAPI(callNode);
        }
    }
}

bool BufOverflowDetector::relationalUpperBound(const ICFGNode* node,
        const ObjVar* object, const GepStmt* gep, s64_t width)
{
    auto& ae = AbstractInterpretation::getAEInstance();
    const BaseObjVar* base = PAG::getPAG()->getBaseObject(object->getId());
    const AllocationSize* size = base ? ae.allocationSizeModel().find(base) : nullptr;
    if (!size || !size->known || size->elementBytes == 0)
        return false;
    // As in the interval check, a base-object target is addressed from its
    // start; a field target adds its recorded offset from the base.
    SVF::Rational baseOffset(0);
    if (const auto* field = SVFUtil::dyn_cast<GepObjVar>(object))
    {
        if (!hasGepObjOffsetFromBase(field))
            return false;
        const SVF::Interval offset = getGepObjOffsetFromBase(field);
        if (!offset.upper().isFinite())
            return false;
        baseOffset = offset.upper().value();
    }
    s64_t constant = 0, scale = 0;
    const ValVar* index = nullptr;
    if (!decomposeGepOffset(gep, constant, index, scale) || !index ||
            scale != static_cast<s64_t>(size->elementBytes) ||
            index->isPointer() || !ae.adapter_.contains(*index))
        return false;
    const auto& state = ae.state(node);
    if (state.isBottom())
        return false;
    const SVF::Variable indexVariable = ae.adapter_.variable(*index);
    const SVF::Variable sizeVariable = ae.adapter_.sizeVariable(*base);
    if (state.numericalMayBeUninitialized(indexVariable) ||
            state.numericalMayBeUninitialized(sizeVariable))
        return false;
    const SVF::Interval difference = state.numerical().bound(
                                         SVF::LinearExpression(indexVariable) - SVF::LinearExpression(sizeVariable));
    if (std::getenv("SVF_AE_TRACE_SIZE"))
        std::cerr << "AE_SIZE_CHECK node=" << node->getId() << " object=" << base->getId()
                  << " index=" << index->getId() << " difference=" << difference.toString()
                  << " size=" << state.numerical().bound(sizeVariable).toString()
                  << " idx=" << state.numerical().bound(indexVariable).toString() << '\n';
    if (difference.isBottom() || !difference.upper().isFinite())
        return false;
    // scale * (idx - Size) + baseOffset + constant + width <= 0
    const SVF::Rational limit =
        ((SVF::Rational(0) - baseOffset - SVF::Rational(constant + width)) /
         SVF::Rational(scale)).floor();
    return difference.upper().value() <= limit;
}

bool BufOverflowDetector::relationalLengthBound(const CallICFGNode* call,
        const ValVar* pointer, const ValVar* length)
{
    auto& ae = AbstractInterpretation::getAEInstance();
    if (!length || length->isPointer() || !ae.adapter_.contains(*length))
        return false;
    const auto lengthInterval = ae.getInterval(length, call);
    if (lengthInterval.isBottom() || !lengthInterval.lower().isFinite() ||
            lengthInterval.lower().value() < SVF::Rational(0))
        return false;
    const auto targets = ae.getAddressSet(pointer, call);
    if (targets.isBottom() || !targets.isFinite() || targets.hasUnknownObject())
        return false;
    const auto& state = ae.state(call);
    const auto lengthVariable = ae.adapter_.variable(*length);
    if (state.isBottom() || state.numericalMayBeUninitialized(lengthVariable))
        return false;
    bool checked = false;
    for (const auto location : targets)
    {
        const auto* object = ae.objectAt(location);
        const auto* base = object ? PAG::getPAG()->getBaseObject(object->getId()) : nullptr;
        const AllocationSize* size = base ? ae.allocationSizeModel().find(base) : nullptr;
        // Library lengths are bytes. Other element units need a scaled
        // inequality, which Octagon cannot generally represent.
        if (!size || !size->known || size->elementBytes != 1)
            return false;
        SVF::Rational offset(0);
        if (const auto* field = SVFUtil::dyn_cast<GepObjVar>(object))
        {
            if (!hasGepObjOffsetFromBase(field))
                return false;
            const auto range = getGepObjOffsetFromBase(field);
            if (range.isBottom() || !range.lower().isFinite() ||
                    range.lower().value() < SVF::Rational(0) || !range.upper().isFinite())
                return false;
            offset = range.upper().value();
        }
        const auto sizeVariable = ae.adapter_.sizeVariable(*base);
        if (state.numericalMayBeUninitialized(sizeVariable))
            return false;
        const auto difference = state.numerical().bound(
                                    SVF::LinearExpression(lengthVariable) - SVF::LinearExpression(sizeVariable));
        if (std::getenv("SVF_AE_TRACE_SIZE"))
            std::cerr << "AE_SIZE_LENGTH node=" << call->getId()
                      << " difference=" << difference.toString()
                      << " length=" << lengthInterval.toString()
                      << " size=" << state.numerical().bound(sizeVariable).toString()
                      << " offset=" << offset.toString() << '\n';
        if (difference.isBottom() || !difference.upper().isFinite() ||
                difference.upper().value() > SVF::Rational(0) - offset)
            return false;
        checked = true;
    }
    return checked;
}

void BufOverflowDetector::detectMemoryBounds(const SVFStmt* statement,
        const SVFVar* pointer, const std::string& kind)
{
    auto& ae = AbstractInterpretation::getAEInstance();
    using Outcome = AbstractInterpretation::QueryOutcome;
    const auto* node = statement->getICFGNode();
    const auto targets = ae.getAddressSet(pointer, node);
    bool unsupported = statement->getAccessBytes() == 0;
    bool may = targets.isBottom() || !targets.isFinite() || targets.hasUnknownObject();
    bool checked = false;
    bool relational = false;
    const s64_t accessBytes = static_cast<s64_t>(statement->getAccessBytes());
    // D-005 item 7: a pointer formed by a GEP can be proved in bounds through
    // the relation idx - Size(o) of that GEP's base objects.
    const auto* gep = pointer && pointer->getInEdges().size() == 1
                      ? SVFUtil::dyn_cast<GepStmt>(*pointer->getInEdges().begin())
                      : nullptr;
    const auto provedThroughGep = [&](const BaseObjVar* base)
    {
        if (!gep)
            return false;
        const SVF::AddressSet sources = ae.getAddressSet(gep->getRHSVar(), node);
        if (sources.isBottom() || !sources.isFinite() || sources.hasUnknownObject())
            return false;
        bool any = false;
        for (const auto location : sources)
        {
            const auto* source = ae.objectAt(location);
            if (!source || PAG::getPAG()->getBaseObject(source->getId()) != base ||
                    !relationalUpperBound(node, source, gep, accessBytes))
                return false;
            any = true;
        }
        return any;
    };
    if (targets.isFinite()) for (const auto location : targets)
        {
            const auto* object = ae.objectAt(location);
            const auto* base = object ? PAG::getPAG()->getBaseObject(object->getId()) : nullptr;
            if (!base || base->isBlackHoleObj())
            {
                may = true;
                continue;
            }
            const auto lower = ae.allocationByteLowerBound(base);
            if (!lower)
            {
                unsupported = true;
                continue;
            }
            SVF::Interval offset = integerInterval(0);
            if (const auto* field = SVFUtil::dyn_cast<GepObjVar>(object))
            {
                if (!hasGepObjOffsetFromBase(field))
                {
                    unsupported = true;
                    continue;
                }
                offset = getGepObjOffsetFromBase(field);
            }
            checked = true;
            const SVF::Rational size(*lower);
            const SVF::Rational width(accessBytes);
            const bool lowerSafe = offset.lower().isFinite() &&
                                   offset.lower().value() >= SVF::Rational(0);
            if (lowerSafe && offset.upper().isFinite() && offset.upper().value() + width <= size)
                continue;
            if (lowerSafe && provedThroughGep(base))
            {
                relational = true;
                continue;
            }
            const AllocationSize* model = ae.allocationSizeModel().find(base);
            if (*lower == 0 && !(model && model->known && model->sizeValue))
            {
                // Static zero or unsized extents (extern T[0]) stay unsupported;
                // a dynamic size whose lower bound is 0 is a genuine May.
                unsupported = true;
                continue;
            }
            may = true;
        }
    ae.recordQuery(BUF_OVERFLOW, node, pointer, kind,
                   may ? Outcome::May : (unsupported || !checked ? Outcome::Unsupported : Outcome::Safe),
                   may ? "memory access may exceed allocation byte range"
                   : (unsupported || !checked ? "access width, allocation size or offset unsupported"
                      : relational
                      ? "access byte range is within allocation; nullness/lifetime checked separately [relational-size]"
                      : "access byte range is within allocation; nullness/lifetime checked separately"), statement);
}

void BufOverflowDetector::enumerateQueries()
{
    auto& ae = AbstractInterpretation::getAEInstance();
    ICFG* graph = PAG::getPAG()->getICFG();
    for (auto iterator = graph->begin(); iterator != graph->end(); ++iterator)
    {
        const ICFGNode* node = iterator->second;
        if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        {
            const FunObjVar* function = call->getCalledFunction();
            if (!function)
                continue;
            const std::string name = function->getName();
            if ((name == "SAFE_BUFACCESS" || name == "UNSAFE_BUFACCESS") &&
                    call->arg_size() >= 2)
                ae.registerQuery(BUF_OVERFLOW, call, call->getArgument(0),
                                 "stub-bounds");
            if (!SVFUtil::isExtCall(function))
                continue;

            AbsExtAPI::ExtAPIType extType = AbsExtAPI::UNCLASSIFIED;
            for (const std::string& annotation :
                    ExtAPI::getExtAPI()->getExtFuncAnnotations(function))
            {
                if (annotation.find("MEMCPY") != std::string::npos)
                    extType = AbsExtAPI::MEMCPY;
                if (annotation.find("MEMSET") != std::string::npos)
                    extType = AbsExtAPI::MEMSET;
                if (annotation.find("STRCPY") != std::string::npos)
                    extType = AbsExtAPI::STRCPY;
                if (annotation.find("STRCAT") != std::string::npos)
                    extType = AbsExtAPI::STRCAT;
            }

            if (extType == AbsExtAPI::MEMCPY ||
                    extType == AbsExtAPI::MEMSET)
            {
                auto rule = extAPIBufOverflowCheckRules.find(name);
                if (rule == extAPIBufOverflowCheckRules.end())
                {
                    ae.registerQuery(BUF_OVERFLOW, call, nullptr,
                                     "ext-bounds-unsupported-rule");
                    continue;
                }
                for (const auto& argument : rule->second)
                {
                    const ValVar* pointer = argument.first < call->arg_size()
                                            ? call->getArgument(argument.first)
                                            : nullptr;
                    ae.registerQuery(
                        BUF_OVERFLOW, call, pointer,
                        "ext-bounds-arg-" +
                        std::to_string(argument.first) + "-len-" +
                        std::to_string(argument.second));
                }
            }
            else if ((extType == AbsExtAPI::STRCPY ||
                      extType == AbsExtAPI::STRCAT) && call->arg_size() >= 1)
                ae.registerQuery(BUF_OVERFLOW, call, call->getArgument(0),
                                 "ext-string-destination");
            continue;
        }

        for (const SVFStmt* statement : node->getSVFStmts())
        {
            if (const auto* gep = SVFUtil::dyn_cast<GepStmt>(statement))
                ae.registerQuery(BUF_OVERFLOW, node, gep->getRHSVar(),
                                 "gep-allocation-range", gep);
            else if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement))
                ae.registerQuery(BUF_OVERFLOW, node, load->getRHSVar(), "load-bounds", load);
            else if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(statement))
            {
                if (node->getFun())
                    ae.registerQuery(BUF_OVERFLOW, node, store->getLHSVar(), "store-bounds", store);
            }
        }
    }
}

/**
 * @brief Handles stub functions within the ICFG node.
 *
 * This function is a placeholder for handling stub functions within the ICFG
 * node.
 *
 * @param node Pointer to the ICFG node.
 */
void BufOverflowDetector::handleStubFunctions(const SVF::CallICFGNode* callNode)
{
    // get function name
    std::string funcName = callNode->getCalledFunction()->getName();
    auto& ae = AbstractInterpretation::getAEInstance();
    if (funcName == "SAFE_BUFACCESS")
    {
        ae.getUtils()->checkpoints.erase(callNode);
        if (callNode->arg_size() < 2)
            return;
        SVF::Interval val = ae.getInterval(callNode->getArgument(1), callNode);
        if (val.isBottom())
        {
            val = integerInterval(0);
            assert(false && "SAFE_BUFACCESS size is bottom");
        }
        const ValVar* arg0Val = callNode->getArgument(0);
        bool isSafe = canSafelyAccessMemory(arg0Val, val, callNode);
        ae.recordQuery(BUF_OVERFLOW, callNode, arg0Val, "stub-bounds",
                       isSafe ? AbstractInterpretation::QueryOutcome::Safe
                       : AbstractInterpretation::QueryOutcome::May,
                       isSafe ? "access is in bounds"
                       : "access may exceed object bounds");
        if (isSafe)
        {
            SVFUtil::outs()
                    << SVFUtil::sucMsg(
                        "success: expected safe buffer access at SAFE_BUFACCESS")
                    << " — " << callNode->toString() << "\n";
            return;
        }
        else
        {
            SVFUtil::outs()
                    << SVFUtil::errMsg(
                        "failure: unexpected buffer overflow at SAFE_BUFACCESS")
                    << " — Position: " << callNode->getSourceLoc() << "\n";
            assert(false);
        }
    }
    else if (funcName == "UNSAFE_BUFACCESS")
    {
        ae.getUtils()->checkpoints.erase(callNode);
        if (callNode->arg_size() < 2)
            return;
        SVF::Interval val = ae.getInterval(callNode->getArgument(1), callNode);
        if (val.isBottom())
        {
            assert(false && "UNSAFE_BUFACCESS size is bottom");
        }
        const ValVar* arg0Val = callNode->getArgument(0);
        bool isSafe = canSafelyAccessMemory(arg0Val, val, callNode);
        ae.recordQuery(BUF_OVERFLOW, callNode, arg0Val, "stub-bounds",
                       isSafe ? AbstractInterpretation::QueryOutcome::Safe
                       : AbstractInterpretation::QueryOutcome::May,
                       isSafe ? "access is in bounds"
                       : "access may exceed object bounds");
        if (!isSafe)
        {
            SVFUtil::outs()
                    << SVFUtil::sucMsg(
                        "success: expected buffer overflow at UNSAFE_BUFACCESS")
                    << " — " << callNode->toString() << "\n";
            return;
        }
        else
        {
            SVFUtil::outs()
                    << SVFUtil::errMsg("failure: buffer overflow expected at "
                               "UNSAFE_BUFACCESS, but none detected")
                    << " — Position: " << callNode->getSourceLoc() << "\n";
            assert(false);
        }
    }
}

/**
 * @brief Initializes external API buffer overflow check rules.
 *
 * This function sets up rules for various memory-related functions like memcpy,
 * memset, etc., defining which arguments should be checked for buffer
 * overflows.
 */
void BufOverflowDetector::initExtAPIBufOverflowCheckRules()
{
    extAPIBufOverflowCheckRules["llvm_memcpy_p0i8_p0i8_i64"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["llvm_memcpy_p0_p0_i64"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["llvm_memcpy_p0i8_p0i8_i32"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["llvm_memcpy"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["llvm_memmove"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["llvm_memmove_p0i8_p0i8_i64"] = {{0, 2},
        {1, 2}
    };
    extAPIBufOverflowCheckRules["llvm_memmove_p0_p0_i64"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["llvm_memmove_p0i8_p0i8_i32"] = {{0, 2},
        {1, 2}
    };
    extAPIBufOverflowCheckRules["__memcpy_chk"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["memcpy"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["memmove"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["bcopy"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["memccpy"] = {{0, 3}, {1, 3}};
    extAPIBufOverflowCheckRules["__memmove_chk"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["llvm_memset"] = {{0, 2}};
    extAPIBufOverflowCheckRules["llvm_memset_p0i8_i32"] = {{0, 2}};
    extAPIBufOverflowCheckRules["llvm_memset_p0i8_i64"] = {{0, 2}};
    extAPIBufOverflowCheckRules["llvm_memset_p0_i64"] = {{0, 2}};
    extAPIBufOverflowCheckRules["__memset_chk"] = {{0, 2}};
    extAPIBufOverflowCheckRules["wmemset"] = {{0, 2}};
    extAPIBufOverflowCheckRules["strncpy"] = {{0, 2}, {1, 2}};
    extAPIBufOverflowCheckRules["iconv"] = {{1, 2}, {3, 4}};
}

/**
 * @brief Handles external API calls related to buffer overflow detection.
 *
 * This function checks the type of external memory API (e.g., memcpy, memset,
 * strcpy, strcat) and applies the corresponding buffer overflow checks based on
 * predefined rules.
 *
 * @param call Pointer to the call ICFG node.
 */
void BufOverflowDetector::detectExtAPI(const CallICFGNode* call)
{
    BufferOverflowInfo info;
    assert(call->getCalledFunction() && "FunObjVar* is nullptr");
    auto& ae = AbstractInterpretation::getAEInstance();

    AbsExtAPI::ExtAPIType extType = AbsExtAPI::UNCLASSIFIED;

    // Determine the type of external memory API
    for (const std::string& annotation :
            ExtAPI::getExtAPI()->getExtFuncAnnotations(call->getCalledFunction()))
    {
        if (annotation.find("MEMCPY") != std::string::npos)
            extType = AbsExtAPI::MEMCPY;
        if (annotation.find("MEMSET") != std::string::npos)
            extType = AbsExtAPI::MEMSET;
        if (annotation.find("STRCPY") != std::string::npos)
            extType = AbsExtAPI::STRCPY;
        if (annotation.find("STRCAT") != std::string::npos)
            extType = AbsExtAPI::STRCAT;
    }

    // Apply buffer overflow checks based on the determined API type
    if (extType == AbsExtAPI::MEMCPY)
    {
        if (extAPIBufOverflowCheckRules.count(
                    call->getCalledFunction()->getName()) == 0)
        {
            ae.recordQuery(BUF_OVERFLOW, call, nullptr,
                           "ext-bounds-unsupported-rule",
                           AbstractInterpretation::QueryOutcome::Unsupported,
                           "external API has no buffer-size rule");
            SVFUtil::errs()
                    << "Warning: " << call->getCalledFunction()->getName()
                    << " is not in the rules, please implement it\n";
            return;
        }
        std::vector<std::pair<u32_t, u32_t>> args =
            extAPIBufOverflowCheckRules.at(
                call->getCalledFunction()->getName());
        for (auto arg : args)
        {
            const ValVar* argVar = arg.first < call->arg_size()
                                   ? call->getArgument(arg.first) : nullptr;
            if (arg.first >= call->arg_size() ||
                    arg.second >= call->arg_size())
            {
                ae.recordQuery(
                    BUF_OVERFLOW, call, argVar,
                    "ext-bounds-arg-" + std::to_string(arg.first) +
                    "-len-" + std::to_string(arg.second),
                    AbstractInterpretation::QueryOutcome::Unsupported,
                    "external API call does not match buffer-size rule");
                continue;
            }
            SVF::Interval offset = SVF::subtract(
                                       ae.getInterval(call->getArgument(arg.second), call),
                                       integerInterval(1));
            const bool intervalSafe = canSafelyAccessMemory(argVar, offset, call, &info);
            const bool relational = !intervalSafe && relationalLengthBound(
                                        call, argVar, call->getArgument(arg.second));
            const bool safe = intervalSafe || relational;
            ae.recordQuery(BUF_OVERFLOW, call, argVar,
                           "ext-bounds-arg-" + std::to_string(arg.first) +
                           "-len-" + std::to_string(arg.second),
                           safe ? AbstractInterpretation::QueryOutcome::Safe
                           : AbstractInterpretation::QueryOutcome::May,
                           safe ? (relational ? "access is in bounds [relational-size]" : "access is in bounds")
                           : "access may exceed object bounds");
            if (!safe)
            {
                AEException bug(call->toString());
                addBugToReporter(bug, call, info);
            }
        }
    }
    else if (extType == AbsExtAPI::MEMSET)
    {
        if (extAPIBufOverflowCheckRules.count(
                    call->getCalledFunction()->getName()) == 0)
        {
            ae.recordQuery(BUF_OVERFLOW, call, nullptr,
                           "ext-bounds-unsupported-rule",
                           AbstractInterpretation::QueryOutcome::Unsupported,
                           "external API has no buffer-size rule");
            SVFUtil::errs()
                    << "Warning: " << call->getCalledFunction()->getName()
                    << " is not in the rules, please implement it\n";
            return;
        }
        std::vector<std::pair<u32_t, u32_t>> args =
            extAPIBufOverflowCheckRules.at(
                call->getCalledFunction()->getName());
        for (auto arg : args)
        {
            const ValVar* argVar = arg.first < call->arg_size()
                                   ? call->getArgument(arg.first) : nullptr;
            if (arg.first >= call->arg_size() ||
                    arg.second >= call->arg_size())
            {
                ae.recordQuery(
                    BUF_OVERFLOW, call, argVar,
                    "ext-bounds-arg-" + std::to_string(arg.first) +
                    "-len-" + std::to_string(arg.second),
                    AbstractInterpretation::QueryOutcome::Unsupported,
                    "external API call does not match buffer-size rule");
                continue;
            }
            SVF::Interval offset = SVF::subtract(
                                       ae.getInterval(call->getArgument(arg.second), call),
                                       integerInterval(1));
            const bool safe = canSafelyAccessMemory(argVar, offset, call, &info);
            ae.recordQuery(BUF_OVERFLOW, call, argVar,
                           "ext-bounds-arg-" + std::to_string(arg.first) +
                           "-len-" + std::to_string(arg.second),
                           safe ? AbstractInterpretation::QueryOutcome::Safe
                           : AbstractInterpretation::QueryOutcome::May,
                           safe ? "access is in bounds"
                           : "access may exceed object bounds");
            if (!safe)
            {
                AEException bug(call->toString());
                addBugToReporter(bug, call, info);
            }
        }
    }
    else if (extType == AbsExtAPI::STRCPY)
    {
        const bool safe = detectStrcpy(call, info);
        const ValVar* destination = call->arg_size() >= 1
                                    ? call->getArgument(0) : nullptr;
        ae.recordQuery(BUF_OVERFLOW, call, destination,
                       "ext-string-destination",
                       safe ? AbstractInterpretation::QueryOutcome::Safe
                       : AbstractInterpretation::QueryOutcome::May,
                       safe ? "destination is large enough"
                       : "destination may be too small");
        if (!safe)
        {
            AEException bug(call->toString());
            addBugToReporter(bug, call, info);
        }
    }
    else if (extType == AbsExtAPI::STRCAT)
    {
        const bool safe = detectStrcat(call, info);
        const ValVar* destination = call->arg_size() >= 1
                                    ? call->getArgument(0) : nullptr;
        ae.recordQuery(BUF_OVERFLOW, call, destination,
                       "ext-string-destination",
                       safe ? AbstractInterpretation::QueryOutcome::Safe
                       : AbstractInterpretation::QueryOutcome::May,
                       safe ? "destination is large enough"
                       : "destination may be too small");
        if (!safe)
        {
            AEException bug(call->toString());
            addBugToReporter(bug, call, info);
        }
    }
    else
    {
        // Handle other cases
    }
}

/**
 * @brief Retrieves the access offset for a given object and GEP statement.
 *
 * This function calculates the access offset for a base object or a sub-object
 * of an aggregate object (using GEP). If the object is a dummy object, it
 * returns a top interval value.
 *
 * @param objId The ID of the object.
 * @param gep Pointer to the GEP statement.
 * @return The interval value of the access offset.
 */
SVF::Interval BufOverflowDetector::getAccessOffset(SVF::NodeID objId,
        const SVF::GepStmt* gep)
{
    SVFIR* svfir = PAG::getPAG();
    auto obj = svfir->getSVFVar(objId);

    if (SVFUtil::isa<BaseObjVar>(obj))
    {
        return detectorGepOffset(gep);
    }
    else if (SVFUtil::isa<GepObjVar>(obj))
    {
        return SVF::add(getGepObjOffsetFromBase(SVFUtil::cast<GepObjVar>(obj)),
                        detectorGepOffset(gep));
    }
    else
    {
        assert(SVFUtil::isa<DummyObjVar>(obj) && "Unknown object type");
        return SVF::Interval::top();
    }
}

/**
 * @brief Updates the offset of a GEP object from its base.
 *
 * This function calculates and stores the offset of a GEP object from its base
 * object using the addresses and offsets provided.
 *
 * @param gepAddrs The addresses of the GEP objects.
 * @param objAddrs The addresses of the base objects.
 * @param offset The interval value of the offset.
 */
void BufOverflowDetector::updateGepObjOffsetFromBase(const ICFGNode* node,
        SVF::AddressSet gepAddrs,
        SVF::AddressSet objAddrs,
        SVF::Interval offset)
{
    SVFIR* svfir = PAG::getPAG();
    auto& ae = AbstractInterpretation::getAEInstance();
    (void)node;

    if (gepAddrs.hasUnknownObject() || objAddrs.hasUnknownObject())
        return;

    for (SVF::Location objLocation : objAddrs)
    {
        const ObjVar* mappedObject = ae.objectAt(objLocation);
        if (!mappedObject)
            continue;
        NodeID objId = mappedObject->getId();
        auto obj = svfir->getSVFVar(objId);

        if (SVFUtil::isa<BaseObjVar>(obj))
        {
            // if the object is a BaseObjVar, add the offset directly
            // like llvm bc `arr = alloc i8 12; p = gep arr, 4`
            // we write key value pair {gep, 4}
            for (SVF::Location gepLocation : gepAddrs)
            {
                const ObjVar* mappedGep = ae.objectAt(gepLocation);
                if (!mappedGep)
                    continue;
                NodeID gepObj = mappedGep->getId();
                if (const GepObjVar* gepObjVar =
                            SVFUtil::dyn_cast<GepObjVar>(svfir->getSVFVar(gepObj)))
                {
                    addToGepObjOffsetFromBase(gepObjVar, offset);
                }
            }
        }
        else if (SVFUtil::isa<GepObjVar>(obj))
        {
            // if the object is a GepObjVar, add the offset from the base object
            // like llvm bc `arr = alloc i8 12; p = gep arr, 4; q = gep p, 6`
            // we retreive {p, 4} and write {q, 4+6}
            const GepObjVar* objVar = SVFUtil::cast<GepObjVar>(obj);
            for (SVF::Location gepLocation : gepAddrs)
            {
                const ObjVar* mappedGep = ae.objectAt(gepLocation);
                if (!mappedGep)
                    continue;
                NodeID gepObj = mappedGep->getId();
                if (const GepObjVar* gepObjVar =
                            SVFUtil::dyn_cast<GepObjVar>(svfir->getSVFVar(gepObj)))
                {
                    if (hasGepObjOffsetFromBase(objVar))
                    {
                        SVF::Interval objOffsetFromBase =
                            getGepObjOffsetFromBase(objVar);
                        addToGepObjOffsetFromBase(
                            gepObjVar, SVF::add(objOffsetFromBase, offset));
                    }
                    else
                    {
                        addToGepObjOffsetFromBase(gepObjVar, SVF::Interval::top());
                    }
                }
            }
        }
    }
}

/**
 * @brief Detects buffer overflow in 'strcpy' function calls.
 *
 * This function checks if the destination buffer can safely accommodate the
 * source string being copied, accounting for the null terminator.
 *
 * @param as Reference to the abstract state.
 * @param call Pointer to the call ICFG node.
 * @return True if the memory access is safe, false otherwise.
 */
bool BufOverflowDetector::detectStrcpy(const CallICFGNode* call, BufferOverflowInfo& info)
{
    const ValVar* arg0Val = call->getArgument(0);
    const ValVar* arg1Val = call->getArgument(1);
    auto& ae = AbstractInterpretation::getAEInstance();
    SVF::Interval strLen = ae.getUtils()->getStrlen(arg1Val, call);
    return canSafelyAccessMemory(arg0Val, strLen, call, &info);
}

bool BufOverflowDetector::detectStrcat(const CallICFGNode* call, BufferOverflowInfo& info)
{
    auto& ae = AbstractInterpretation::getAEInstance();
    const std::vector<std::string> strcatGroup = {"__strcat_chk", "strcat",
                                                  "__wcscat_chk", "wcscat"
                                                 };
    const std::vector<std::string> strncatGroup = {"__strncat_chk", "strncat",
                                                   "__wcsncat_chk", "wcsncat"
                                                  };

    if (std::find(strcatGroup.begin(), strcatGroup.end(),
                  call->getCalledFunction()->getName()) != strcatGroup.end())
    {
        const ValVar* arg0Val = call->getArgument(0);
        const ValVar* arg1Val = call->getArgument(1);
        SVF::Interval strLen0 = ae.getUtils()->getStrlen(arg0Val, call);
        SVF::Interval strLen1 = ae.getUtils()->getStrlen(arg1Val, call);
        SVF::Interval totalLen = SVF::add(strLen0, strLen1);
        return canSafelyAccessMemory(arg0Val, totalLen, call, &info);
    }
    else if (std::find(strncatGroup.begin(), strncatGroup.end(),
                       call->getCalledFunction()->getName()) !=
             strncatGroup.end())
    {
        const ValVar* arg0Val = call->getArgument(0);
        const ValVar* arg2Val = call->getArgument(2);
        SVF::Interval arg2Num = ae.getInterval(arg2Val, call);
        SVF::Interval strLen0 = ae.getUtils()->getStrlen(arg0Val, call);
        SVF::Interval totalLen = SVF::add(strLen0, arg2Num);
        return canSafelyAccessMemory(arg0Val, totalLen, call, &info);
    }
    else
    {
        assert(false && "Unknown strcat function, please add it to strcatGroup "
                        "or strncatGroup");
        abort();
    }
}

/**
 * @brief Checks if a memory access is safe given a specific buffer length.
 *
 * This function ensures that a given memory access, starting at a specific
 * value, does not exceed the allocated size of the buffer.
 *
 * @param as Reference to the abstract state.
 * @param value Pointer to the SVF var.
 * @param len The interval value representing the length of the memory access.
 * @return True if the memory access is safe, false otherwise.
 */
bool BufOverflowDetector::canSafelyAccessMemory(const ValVar* value,
        const SVF::Interval& len,
        const ICFGNode* node, BufferOverflowInfo* info)
{
    if (info)
        *info = {};
    SVFIR* svfir = PAG::getPAG();
    auto& ae = AbstractInterpretation::getAEInstance();

    const SVF::AddressSet ptrVal = ae.getAddressSet(value, node);
    if (ptrVal.isBottom() || !ptrVal.isFinite())
        return false;
    for (SVF::Location location : ptrVal)
    {
        const ObjVar* mappedObject = ae.objectAt(location);
        if (!mappedObject)
            return false;
        NodeID objId = mappedObject->getId();
        const BaseObjVar* baseObject = svfir->getBaseObject(objId);
        if (!baseObject)
            return false;
        const auto lower = ae.allocationByteLowerBound(baseObject);
        if (!lower)
            return false;
        const u32_t size = static_cast<u32_t>(std::min<s64_t>(
                *lower, std::numeric_limits<u32_t>::max()));

        SVF::Interval offset = integerInterval(0);
        // if the object is a GepObjVar, get the offset from the base object
        if (SVFUtil::isa<GepObjVar>(svfir->getSVFVar(objId)))
        {
            offset = SVF::add(getGepObjOffsetFromBase(SVFUtil::cast<GepObjVar>(
                    svfir->getSVFVar(objId))),
                              len);
        }
        else if (SVFUtil::isa<BaseObjVar>(svfir->getSVFVar(objId)))
        {
            // if the object is a BaseObjVar, get the offset directly
            offset = len;
        }

        // if the offset is greater than the size, return false
        if (upperAtLeast(offset, size))
        {
            if (info)
                *info = {size, offset};
            return false;
        }
    }
    return true;
}

void NullptrDerefDetector::detect(const ICFGNode* node)
{
    auto& ae = AbstractInterpretation::getAEInstance();
    if (SVFUtil::isa<CallICFGNode>(node))
    {
        // external API like memset(*dst, elem, sz)
        // we check if it's external api and check the corrisponding index
        const CallICFGNode* callNode = SVFUtil::cast<CallICFGNode>(node);
        if (SVFUtil::isExtCall(callNode->getCalledFunction()))
        {
            detectExtAPI(callNode);
        }
    }
    else
    {
        for (const auto& stmt : node->getSVFStmts())
        {
            // GEP forms an address; it does not dereference its operand.
            if (const LoadStmt* load = SVFUtil::dyn_cast<LoadStmt>(stmt))
            {
                // like llvm bitcode `p = load q`
                // The dereferenced pointer is the load address (RHS), not the
                // SSA value produced by the load (LHS).  Querying the LHS
                // reports scalar loads as null-pointer alarms and also misses
                // an unsafe address when the loaded value happens to be a
                // valid pointer.
                const ValVar* address = load->getRHSVar();
                const bool safe = canSafelyDerefPtr(address, node);
                ae.recordQuery(NULL_DEREF, node, address, "load-address",
                               safe ? AbstractInterpretation::QueryOutcome::Safe
                               : AbstractInterpretation::QueryOutcome::May,
                               safe ? "all targets valid"
                               : "may be null, invalid, unknown, or freed");
                if (!safe)
                {
                    AEException bug(stmt->toString());
                    addBugToReporter(bug, stmt->getICFGNode());
                }
            }
            else if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(stmt))
            {
                if (!node->getFun()) continue;
                const auto* address = store->getLHSVar();
                const bool safe = canSafelyDerefPtr(address, node);
                ae.recordQuery(NULL_DEREF, node, address, "store-address",
                               safe ? AbstractInterpretation::QueryOutcome::Safe
                               : AbstractInterpretation::QueryOutcome::May,
                               safe ? "all targets valid"
                               : "may be null, invalid, unknown, or freed", store);
            }
        }
    }
}

void NullptrDerefDetector::enumerateQueries()
{
    auto& ae = AbstractInterpretation::getAEInstance();
    ICFG* graph = PAG::getPAG()->getICFG();
    for (auto iterator = graph->begin(); iterator != graph->end(); ++iterator)
    {
        const ICFGNode* node = iterator->second;
        if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        {
            const FunObjVar* function = call->getCalledFunction();
            if (!function)
                continue;
            const std::string name = function->getName();
            if ((name == "SAFE_LOAD" || name == "UNSAFE_LOAD") &&
                    call->arg_size() >= 1)
                ae.registerQuery(NULL_DEREF, call, call->getArgument(0),
                                 "stub-arg-0");
            if (!SVFUtil::isExtCall(function))
                continue;
            for (u32_t argument : nullDerefArgumentIndices(call))
            {
                if (argument < call->arg_size())
                    ae.registerQuery(NULL_DEREF, call,
                                     call->getArgument(argument),
                                     "ext-arg-" + std::to_string(argument));
            }
            continue;
        }

        for (const SVFStmt* statement : node->getSVFStmts())
        {
            if (const auto* load =
                        SVFUtil::dyn_cast<LoadStmt>(statement))
                ae.registerQuery(NULL_DEREF, node, load->getRHSVar(),
                                 "load-address");
            else if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(statement))
            {
                if (node->getFun())
                    ae.registerQuery(NULL_DEREF, node, store->getLHSVar(), "store-address", store);
            }
        }
    }
}

void NullptrDerefDetector::handleStubFunctions(const CallICFGNode* callNode)
{
    std::string funcName = callNode->getCalledFunction()->getName();
    auto& ae = AbstractInterpretation::getAEInstance();
    if (funcName == "UNSAFE_LOAD")
    {
        // void UNSAFE_LOAD(void* ptr);
        ae.getUtils()->checkpoints.erase(callNode);
        if (callNode->arg_size() < 1)
            return;

        const ValVar* arg0Val = callNode->getArgument(0);
        // opt may directly dereference a null pointer and call
        // UNSAFE_LOAD(null)
        bool isSafe =
            canSafelyDerefPtr(arg0Val, callNode) && arg0Val->getId() != 0;
        ae.recordQuery(NULL_DEREF, callNode, arg0Val, "stub-arg-0",
                       isSafe ? AbstractInterpretation::QueryOutcome::Safe
                       : AbstractInterpretation::QueryOutcome::May,
                       isSafe ? "all targets valid"
                       : "expected unsafe dereference");
        SVFUtil::outs() << "[UNSAFE_LOAD] node=" << callNode->getId()
                        << " arg0=" << arg0Val->getId() << " isSafe=" << isSafe
                        << "\n";
        if (!isSafe)
        {
            SVFUtil::outs()
                    << SVFUtil::sucMsg(
                        "success: expected null dereference at UNSAFE_LOAD")
                    << " — " << callNode->toString() << "\n";
            return;
        }
        else
        {
            SVFUtil::outs()
                    << SVFUtil::errMsg("failure: null dereference expected at "
                               "UNSAFE_LOAD, but none detected")
                    << " — Position: " << callNode->getSourceLoc() << "\n";
            assert(false);
        }
    }
    else if (funcName == "SAFE_LOAD")
    {
        // void SAFE_LOAD(void* ptr);
        ae.getUtils()->checkpoints.erase(callNode);
        if (callNode->arg_size() < 1)
            return;
        const ValVar* arg0Val = callNode->getArgument(0);
        // opt may directly dereference a null pointer and call
        // UNSAFE_LOAD(null)ols
        bool isSafe =
            canSafelyDerefPtr(arg0Val, callNode) && arg0Val->getId() != 0;
        ae.recordQuery(NULL_DEREF, callNode, arg0Val, "stub-arg-0",
                       isSafe ? AbstractInterpretation::QueryOutcome::Safe
                       : AbstractInterpretation::QueryOutcome::May,
                       isSafe ? "all targets valid"
                       : "unexpected unsafe dereference");
        if (isSafe)
        {
            SVFUtil::outs()
                    << SVFUtil::sucMsg(
                        "success: expected safe dereference at SAFE_LOAD")
                    << " — " << callNode->toString() << "\n";
            return;
        }
        else
        {
            SVFUtil::outs()
                    << SVFUtil::errMsg(
                        "failure: unexpected null dereference at SAFE_LOAD")
                    << " — Position: " << callNode->getSourceLoc() << "\n";
            assert(false);
        }
    }
}

void NullptrDerefDetector::detectExtAPI(const CallICFGNode* call)
{
    assert(call->getCalledFunction() && "FunObjVar* is nullptr");
    auto& ae = AbstractInterpretation::getAEInstance();
    for (u32_t arg : nullDerefArgumentIndices(call))
    {
        if (call->arg_size() <= arg)
            continue;
        const ValVar* argVal = call->getArgument(arg);
        if (!argVal)
            continue;
        const bool safe = canSafelyDerefPtr(argVal, call);
        ae.recordQuery(NULL_DEREF, call, argVal,
                       "ext-arg-" + std::to_string(arg),
                       safe ? AbstractInterpretation::QueryOutcome::Safe
                       : AbstractInterpretation::QueryOutcome::May,
                       safe ? "all targets valid"
                       : "may be null, invalid, unknown, or freed");
        if (!safe)
        {
            AEException bug(call->toString());
            addBugToReporter(bug, call);
        }
    }
}

bool NullptrDerefDetector::canSafelyDerefPtr(const ValVar* value,
        const ICFGNode* node)
{
    auto& ae = AbstractInterpretation::getAEInstance();
    const SVF::AddressSet addresses = ae.getAddressSet(value, node);
    if (addresses.isBottom() || !addresses.isFinite())
        return false;
    for (SVF::Location location : addresses)
    {
        if (location.isNull())
            return false;
        const ObjVar* object = ae.objectAt(location);
        const BaseObjVar* base =
            object ? PAG::getPAG()->getBaseObject(object->getId()) : nullptr;
        if (!base || base->isBlackHoleObj() ||
                ae.isFreedMemory(location, node))
            return false;
    }
    return true;
}
