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
#include <AE/Svfexe/AbstractInterpretation.h>

using namespace SVF;
namespace AD = SVF::AbstractDomain;

namespace
{
AD::Interval integerInterval(s64_t value)
{
    return AD::Interval::singleton(AD::Rational(value));
}

bool upperAtLeast(const AD::Interval& interval, u32_t size)
{
    return !interval.upper().isFinite() ||
           interval.upper().value() >= AD::Rational(size);
}

std::vector<u32_t> nullDerefArgumentIndices(const CallICFGNode* call)
{
    std::vector<u32_t> result;
    for (const std::string& annotation :
            ExtAPI::getExtAPI()->getExtFuncAnnotations(call->getCalledFunction()))
    {
        if (annotation.find("MEMCPY") != std::string::npos)
        {
            if (call->arg_size() < 4)
            {
                result.push_back(0);
                result.push_back(1);
            }
            else
            {
                result.push_back(1);
                result.push_back(2);
                result.push_back(3);
                result.push_back(4);
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

                // Update the GEP object offset from its base
                const AD::AddressSet lhsVal =
                    ae.getAddressSet(gep->getLHSVar(), node);
                const AD::AddressSet rhsVal =
                    ae.getAddressSet(gep->getRHSVar(), node);
                updateGepObjOffsetFromBase(node, lhsVal, rhsVal,
                                           ae.getGepByteOffset(gep));

                if (rhsVal.isBottom() || !rhsVal.isFinite() ||
                        rhsVal.hasUnknownObject())
                    unsupported = true;
                else for (AD::Location location : rhsVal)
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
                    u32_t size = 0;
                    bool sizeKnown = false;
                    // like `int arr[10]` which has constant size before runtime
                    if (baseObject->isConstantByteSize())
                    {
                        size = baseObject->getByteSizeOfObj();
                        sizeKnown = true;
                    }
                    else
                    {
                        // like `int len = ***; int arr[len]`, whose size can
                        // only be known in runtime
                        const ICFGNode* addrNode = baseObject->getICFGNode();
                        if (!addrNode)
                        {
                            unsupported = true;
                            continue;
                        }
                        for (const SVFStmt* stmt2 : addrNode->getSVFStmts())
                        {
                            if (const AddrStmt* addrStmt =
                                        SVFUtil::dyn_cast<AddrStmt>(stmt2))
                            {
                                size = ae.getAllocaInstByteSize(addrStmt);
                                sizeKnown = true;
                            }
                        }
                    }

                    if (!sizeKnown)
                        unsupported = true;
                    checkedTarget = true;

                    // Calculate access offset and check for potential overflow
                    AD::Interval accessOffset = getAccessOffset(objId, gep);
                    if (upperAtLeast(accessOffset, size))
                    {
                        mayOverflow = true;
                        AEException bug(stmt->toString());
                        addBugToReporter(bug, stmt->getICFGNode());
                    }
                }
                const auto outcome = mayOverflow
                    ? AbstractInterpretation::QueryOutcome::May
                    : (unsupported || !checkedTarget
                       ? AbstractInterpretation::QueryOutcome::Unsupported
                       : AbstractInterpretation::QueryOutcome::Safe);
                ae.recordQuery(BUF_OVERFLOW, node, gep->getRHSVar(),
                               "gep-bounds", outcome,
                               mayOverflow ? "access may exceed object bounds"
                               : (unsupported || !checkedTarget
                                  ? "object or object size is unsupported"
                                  : "all target offsets are in bounds"));
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
                                 "gep-bounds");
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
        AD::Interval val = ae.getInterval(callNode->getArgument(1), callNode);
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
        AD::Interval val = ae.getInterval(callNode->getArgument(1), callNode);
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
            AD::Interval offset = AD::subtract(
                                      ae.getInterval(call->getArgument(arg.second), call),
                                      integerInterval(1));
            const bool safe = canSafelyAccessMemory(argVar, offset, call);
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
                addBugToReporter(bug, call);
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
            AD::Interval offset = AD::subtract(
                                      ae.getInterval(call->getArgument(arg.second), call),
                                      integerInterval(1));
            const bool safe = canSafelyAccessMemory(argVar, offset, call);
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
                addBugToReporter(bug, call);
            }
        }
    }
    else if (extType == AbsExtAPI::STRCPY)
    {
        const bool safe = detectStrcpy(call);
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
            addBugToReporter(bug, call);
        }
    }
    else if (extType == AbsExtAPI::STRCAT)
    {
        const bool safe = detectStrcat(call);
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
            addBugToReporter(bug, call);
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
AD::Interval BufOverflowDetector::getAccessOffset(SVF::NodeID objId,
        const SVF::GepStmt* gep)
{
    SVFIR* svfir = PAG::getPAG();
    auto& ae = AbstractInterpretation::getAEInstance();
    auto obj = svfir->getSVFVar(objId);

    if (SVFUtil::isa<BaseObjVar>(obj))
    {
        return ae.getGepByteOffset(gep);
    }
    else if (SVFUtil::isa<GepObjVar>(obj))
    {
        return AD::add(getGepObjOffsetFromBase(SVFUtil::cast<GepObjVar>(obj)),
                       ae.getGepByteOffset(gep));
    }
    else
    {
        assert(SVFUtil::isa<DummyObjVar>(obj) && "Unknown object type");
        return AD::Interval::top();
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
        AD::AddressSet gepAddrs,
        AD::AddressSet objAddrs,
        AD::Interval offset)
{
    SVFIR* svfir = PAG::getPAG();
    auto& ae = AbstractInterpretation::getAEInstance();
    (void)node;

    if (gepAddrs.hasUnknownObject() || objAddrs.hasUnknownObject())
        return;

    for (AD::Location objLocation : objAddrs)
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
            for (AD::Location gepLocation : gepAddrs)
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
            for (AD::Location gepLocation : gepAddrs)
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
                        AD::Interval objOffsetFromBase =
                            getGepObjOffsetFromBase(objVar);
                        if (!hasGepObjOffsetFromBase(gepObjVar))
                            addToGepObjOffsetFromBase(
                                gepObjVar, AD::add(objOffsetFromBase, offset));
                    }
                    else
                    {
                        assert(false &&
                               "GEP RHS object has no offset from base");
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
bool BufOverflowDetector::detectStrcpy(const CallICFGNode* call)
{
    const ValVar* arg0Val = call->getArgument(0);
    const ValVar* arg1Val = call->getArgument(1);
    auto& ae = AbstractInterpretation::getAEInstance();
    AD::Interval strLen = ae.getUtils()->getStrlen(arg1Val, call);
    return canSafelyAccessMemory(arg0Val, strLen, call);
}

bool BufOverflowDetector::detectStrcat(const CallICFGNode* call)
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
        AD::Interval strLen0 = ae.getUtils()->getStrlen(arg0Val, call);
        AD::Interval strLen1 = ae.getUtils()->getStrlen(arg1Val, call);
        AD::Interval totalLen = AD::add(strLen0, strLen1);
        return canSafelyAccessMemory(arg0Val, totalLen, call);
    }
    else if (std::find(strncatGroup.begin(), strncatGroup.end(),
                       call->getCalledFunction()->getName()) !=
             strncatGroup.end())
    {
        const ValVar* arg0Val = call->getArgument(0);
        const ValVar* arg2Val = call->getArgument(2);
        AD::Interval arg2Num = ae.getInterval(arg2Val, call);
        AD::Interval strLen0 = ae.getUtils()->getStrlen(arg0Val, call);
        AD::Interval totalLen = AD::add(strLen0, arg2Num);
        return canSafelyAccessMemory(arg0Val, totalLen, call);
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
        const AD::Interval& len,
        const ICFGNode* node)
{
    SVFIR* svfir = PAG::getPAG();
    auto& ae = AbstractInterpretation::getAEInstance();

    const AD::AddressSet ptrVal = ae.getAddressSet(value, node);
    if (ptrVal.isBottom() || !ptrVal.isFinite())
        return false;
    for (AD::Location location : ptrVal)
    {
        const ObjVar* mappedObject = ae.objectAt(location);
        if (!mappedObject)
            return false;
        NodeID objId = mappedObject->getId();
        const BaseObjVar* baseObject = svfir->getBaseObject(objId);
        if (!baseObject)
            return false;
        u32_t size = 0;
        // if the object is a constant size object, get the size directly
        if (baseObject->isConstantByteSize())
        {
            size = baseObject->getByteSizeOfObj();
        }
        else
        {
            // if the object is not a constant size object, get the size from
            // the addrStmt
            const ICFGNode* addrNode = baseObject->getICFGNode();
            if (!addrNode)
                return false;
            for (const SVFStmt* stmt2 : addrNode->getSVFStmts())
            {
                if (const AddrStmt* addrStmt =
                            SVFUtil::dyn_cast<AddrStmt>(stmt2))
                {
                    size = ae.getAllocaInstByteSize(addrStmt);
                }
            }
        }

        AD::Interval offset = integerInterval(0);
        // if the object is a GepObjVar, get the offset from the base object
        if (SVFUtil::isa<GepObjVar>(svfir->getSVFVar(objId)))
        {
            offset = AD::add(getGepObjOffsetFromBase(SVFUtil::cast<GepObjVar>(
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
            if (const GepStmt* gep = SVFUtil::dyn_cast<GepStmt>(stmt))
            {
                // like llvm bitcode `p = gep p, idx`
                // we check rhs p's all address are valid mem
                const ValVar* rhs = gep->getRHSVar();
                const bool safe = canSafelyDerefPtr(rhs, node);
                ae.recordQuery(NULL_DEREF, node, rhs, "gep-address",
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
            else if (const LoadStmt* load = SVFUtil::dyn_cast<LoadStmt>(stmt))
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
            if (const auto* gep = SVFUtil::dyn_cast<GepStmt>(statement))
                ae.registerQuery(NULL_DEREF, node, gep->getRHSVar(),
                                 "gep-address");
            else if (const auto* load =
                         SVFUtil::dyn_cast<LoadStmt>(statement))
                ae.registerQuery(NULL_DEREF, node, load->getRHSVar(),
                                 "load-address");
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
    const AD::AddressSet addresses = ae.getAddressSet(value, node);
    if (addresses.isBottom() || !addresses.isFinite())
        return false;
    for (AD::Location location : addresses)
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
