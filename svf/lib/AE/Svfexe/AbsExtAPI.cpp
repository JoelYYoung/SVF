//===- AbsExtAPI.cpp -- Abstract Interpretation External API handler-----//
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
//  Created on: Sep 9, 2024
//      Author: Xiao Cheng, Jiawei Wang
//
//
#include "AE/Svfexe/AbsExtAPI.h"

#include <cmath>
#include "AE/Svfexe/AbstractInterpretation.h"
#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

#include <algorithm>

using namespace SVF;

namespace
{
SVF::Interval integerInterval(s64_t value)
{
    return SVF::Interval::singleton(SVF::Rational(value));
}

s64_t lowerInteger(const SVF::Interval& interval)
{
    return interval.lower().value().toInt64();
}
} // namespace
AbsExtAPI::AbsExtAPI(AbstractInterpretation* ae) : ae(ae)
{
    svfir = PAG::getPAG();
    icfg = svfir->getICFG();
    initExtFunMap();
}

void AbsExtAPI::initExtFunMap()
{
#define SSE_FUNC_PROCESS(LLVM_NAME, FUNC_NAME)                                 \
    auto sse_##FUNC_NAME = [this](const CallICFGNode* callNode) {              \
        assert(callNode->arg_size() >= 1 && "external function expects one argument"); \
        const SVFVar* argVar = callNode->getArgument(0);                       \
        const SVF::Interval argVal = ae->getInterval(argVar, callNode);         \
        SVF::Interval result = SVF::Interval::top();                          \
        if (argVal.isSingleton()) {                                           \
            u32_t rhs = lowerInteger(argVal);                                \
            result = integerInterval(FUNC_NAME(rhs));                         \
        }                                                                    \
        const SVFVar* retVar = callNode->getRetICFGNode()->getActualRet();     \
        ae->updateInterval(retVar, result, callNode);                         \
    };                                                                       \
    func_map[#FUNC_NAME] = sse_##FUNC_NAME;

    SSE_FUNC_PROCESS(isalnum, isalnum);
    SSE_FUNC_PROCESS(isalpha, isalpha);
    SSE_FUNC_PROCESS(isblank, isblank);
    SSE_FUNC_PROCESS(iscntrl, iscntrl);
    SSE_FUNC_PROCESS(isdigit, isdigit);
    SSE_FUNC_PROCESS(isgraph, isgraph);
    SSE_FUNC_PROCESS(isprint, isprint);
    SSE_FUNC_PROCESS(ispunct, ispunct);
    SSE_FUNC_PROCESS(isspace, isspace);
    SSE_FUNC_PROCESS(isupper, isupper);
    SSE_FUNC_PROCESS(isxdigit, isxdigit);
    SSE_FUNC_PROCESS(llvm.sin.f64, sin);
    SSE_FUNC_PROCESS(llvm.cos.f64, cos);
    SSE_FUNC_PROCESS(llvm.tan.f64, tan);
    SSE_FUNC_PROCESS(llvm.log.f64, log);
    SSE_FUNC_PROCESS(sinh, sinh);
    SSE_FUNC_PROCESS(cosh, cosh);
    SSE_FUNC_PROCESS(tanh, tanh);

    auto sse_svf_assert = [this](const CallICFGNode* callNode)
    {
        checkpoints.erase(callNode);
        if (ae->isAssertionTrue(callNode->getArgument(0), callNode))
        {
            SVFUtil::errs() << SVFUtil::sucMsg(
                                "The assertion is successfully verified!!\n");
        }
        else
        {
            SVFUtil::errs()
                    << SVFUtil::errMsg("Assertion failure, this svf_assert cannot "
                               "be verified!!\n")
                    << callNode->toString() << "\n";
            // A query-ledger run records this as May and must continue so a
            // complete dataset can be classified. Preserve the historical
            // assertion behavior when no ledger was requested.
            if (!ae->queryLedgerEnabled())
                assert(false);
        }
        return;
    };
    func_map["svf_assert"] = sse_svf_assert;

    auto svf_assert_eq = [this](const CallICFGNode* callNode)
    {
        const SVF::Interval arg0Val =
            ae->getInterval(callNode->getArgument(0), callNode);
        const SVF::Interval arg1Val =
            ae->getInterval(callNode->getArgument(1), callNode);
        if (arg0Val == arg1Val)
        {
            SVFUtil::errs() << SVFUtil::sucMsg(
                                "The assertion is successfully verified!!\n");
        }
        else
        {
            SVFUtil::errs()
                    << "svf_assert_eq Fail. " << callNode->toString() << "\n";
            assert(false);
        }
        return;
    };
    func_map["svf_assert_eq"] = svf_assert_eq;

    auto svf_print = [&](const CallICFGNode* callNode)
    {
        assert(callNode->arg_size() >= 2 && "svf_print expects two arguments");
        std::string text = strRead(callNode->getArgument(1), callNode);
        SVF::Interval itv = ae->getInterval(callNode->getArgument(0), callNode);
        std::cout << "Text: " << text
                  << ", Value: " << callNode->getArgument(0)->toString()
                  << ", PrintVal: " << itv.toString()
                  << ", Loc:" << callNode->getSourceLoc() << std::endl;
        return;
    };
    func_map["svf_print"] = svf_print;

    auto svf_set_value = [&](const CallICFGNode* callNode)
    {
        assert(callNode->arg_size() >= 3 && "set_value expects three arguments");
        const SVF::Interval lbVal =
            ae->getInterval(callNode->getArgument(1), callNode);
        const SVF::Interval ubVal =
            ae->getInterval(callNode->getArgument(2), callNode);
        assert(lbVal.isSingleton() && ubVal.isSingleton());
        SVF::Interval num = SVF::Interval::closed(lbVal.singletonValue(),
            ubVal.singletonValue());
        ae->updateInterval(callNode->getArgument(0), num, callNode);
        const ICFGNode* node =
            SVFUtil::cast<ValVar>(callNode->getArgument(0))->getICFGNode();
        for (const SVFStmt* stmt : node->getSVFStmts())
        {
            if (SVFUtil::isa<LoadStmt>(stmt))
            {
                const LoadStmt* load = SVFUtil::cast<LoadStmt>(stmt);
                const SVF::AddressSet ptrVal =
                    ae->getAddressSet(load->getRHSVar(), callNode);
                if (ptrVal.hasUnknownObject())
                    continue;
                for (SVF::Location location : ptrVal)
                    ae->updateMemoryValue(location, num,
                                          SVF::AddressSet::bottom(), callNode);
            }
        }
        return;
    };
    func_map["set_value"] = svf_set_value;

    auto sse_fread = [&](const CallICFGNode* callNode)
    {
        assert(callNode->arg_size() >= 3 && "fread expects at least three arguments");
        SVF::Interval block_count =
            ae->getInterval(callNode->getArgument(2), callNode);
        SVF::Interval block_size =
            ae->getInterval(callNode->getArgument(1), callNode);
        SVF::Interval block_byte = SVF::multiply(block_count, block_size);
        (void)block_byte;
    };
    func_map["fread"] = sse_fread;

    auto sse_sprintf = [&](const CallICFGNode* callNode)
    {
        // printf is difficult to predict since it has no byte size arguments
    };

    auto sse_snprintf = [&](const CallICFGNode* callNode)
    {
        assert(callNode->arg_size() >= 2 && "formatted-output function expects at least two arguments");
        // get elem size of arg2
        u32_t elemSize = 1;
        if (callNode->arg_size() > 2 &&
                callNode->getArgument(2)->getType()->isArrayTy())
        {
            elemSize = SVFUtil::dyn_cast<SVFArrayType>(
                           callNode->getArgument(2)->getType())
                       ->getTypeOfElement()
                       ->getByteSize();
        }
        SVF::Interval size = SVF::subtract(
                                 SVF::multiply(ae->getInterval(callNode->getArgument(1), callNode),
                                               integerInterval(elemSize)),
                                 integerInterval(1));
        (void)size;
    };
    func_map["__snprintf_chk"] = sse_snprintf;
    func_map["__vsprintf_chk"] = sse_sprintf;
    func_map["__sprintf_chk"] = sse_sprintf;
    func_map["snprintf"] = sse_snprintf;
    func_map["sprintf"] = sse_sprintf;
    func_map["vsprintf"] = sse_sprintf;
    func_map["vsnprintf"] = sse_snprintf;
    func_map["__vsnprintf_chk"] = sse_snprintf;
    func_map["swprintf"] = sse_snprintf;
    func_map["_snwprintf"] = sse_snprintf;

    auto sse_itoa = [&](const CallICFGNode* callNode)
    {
        assert(callNode->arg_size() >= 3 && "itoa expects three arguments");
        const SVF::Interval value =
            ae->getInterval(callNode->getArgument(0), callNode);
        if (!value.isSingleton())
            return;
        u32_t num = static_cast<u32_t>(value.singletonValue().toInt64());
        std::string snum = std::to_string(num);
        (void)snum;
    };
    func_map["itoa"] = sse_itoa;

    auto sse_strlen = [&](const CallICFGNode* callNode)
    {
        assert(callNode->arg_size() >= 1 && "strlen expects one argument");
        const SVFVar* retVar = callNode->getRetICFGNode()->getActualRet();
        SVF::Interval byteLen = getStrlen(callNode->getArgument(0), callNode);
        u32_t elemSize = getElementSize(callNode->getArgument(0));
        if (byteLen.isSingleton() && elemSize > 1)
            ae->updateInterval(
                retVar,
                integerInterval(byteLen.singletonValue().toInt64() /
                                static_cast<s64_t>(elemSize)),
                callNode);
        else
            ae->updateInterval(retVar, byteLen, callNode);
    };
    func_map["strlen"] = sse_strlen;
    func_map["wcslen"] = sse_strlen;

    auto sse_recv = [&](const CallICFGNode* callNode)
    {
        assert(callNode->arg_size() >= 4 && "recv expects four arguments");
        SVF::Interval len =
            SVF::subtract(ae->getInterval(callNode->getArgument(2), callNode),
                          integerInterval(1));
        const SVFVar* retVar = callNode->getRetICFGNode()->getActualRet();
        ae->updateInterval(retVar, len, callNode);
    };
    func_map["recv"] = sse_recv;
    func_map["__recv"] = sse_recv;

    auto sse_free = [&](const CallICFGNode* callNode)
    {
        assert(callNode->arg_size() >= 1 && "free expects one argument");
        const SVF::AddressSet ptrVal =
            ae->getAddressSet(callNode->getArgument(0), callNode);
        if (ptrVal.hasUnknownObject())
            return;
        for (SVF::Location location : ptrVal)
        {
            if (!location.isNull())
                ae->markFreedMemory(location, callNode);
        }
    };
    // Add all free-related functions to func_map
    std::vector<std::string> freeFunctions =
    {
        "VOS_MemFree",       "cfree",        "free",
        "free_all_mem",      "freeaddrinfo", "gcry_mpi_release",
        "gcry_sexp_release", "globfree",     "nhfree",
        "obstack_free",      "safe_cfree",   "safe_free",
        "safefree",          "safexfree",    "sm_free",
        "vim_free",          "xfree",        "SSL_CTX_free",
        "SSL_free",          "XFree"
    };

    for (const auto& name : freeFunctions)
    {
        func_map[name] = sse_free;
    }
};

void AbsExtAPI::collectCheckPoint()
{
    // traverse every ICFGNode
    Set<std::string> ae_checkpoint_names = {"svf_assert"};
    Set<std::string> buf_checkpoint_names = {"UNSAFE_BUFACCESS",
                                             "SAFE_BUFACCESS"
                                            };
    Set<std::string> nullptr_checkpoint_names = {"UNSAFE_LOAD", "SAFE_LOAD"};

    for (auto it = svfir->getICFG()->begin(); it != svfir->getICFG()->end();
            ++it)
    {
        const ICFGNode* node = it->second;
        if (const CallICFGNode* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        {
            if (const FunObjVar* fun = call->getCalledFunction())
            {
                if (ae_checkpoint_names.find(fun->getName()) !=
                        ae_checkpoint_names.end())
                {
                    checkpoints.insert(call);
                }
                if (Options::BufferOverflowCheck())
                {
                    if (buf_checkpoint_names.find(fun->getName()) !=
                            buf_checkpoint_names.end())
                    {
                        checkpoints.insert(call);
                    }
                }
                if (Options::NullDerefCheck())
                {
                    if (nullptr_checkpoint_names.find(fun->getName()) !=
                            nullptr_checkpoint_names.end())
                    {
                        checkpoints.insert(call);
                    }
                }
            }
        }
    }
}

void AbsExtAPI::checkPointAllSet()
{
    if (checkpoints.size() == 0)
    {
        return;
    }
    else
    {
        SVFUtil::errs() << SVFUtil::errMsg(
                            "At least one svf_assert has not been checked!!")
                        << "\n";
        for (const CallICFGNode* call : checkpoints)
            SVFUtil::errs() << call->toString() + "\n";
        assert(false);
    }
}

std::string AbsExtAPI::strRead(const ValVar* rhs, const ICFGNode* node)
{
    std::string str0;

    for (u32_t index = 0; index < Options::MaxFieldLimit(); index++)
    {
        const SVF::AddressSet expression =
            ae->getGepObjAddrs(rhs, integerInterval(index), node);
        if (!expression.isFinite() || expression.isBottom())
            break;
        SVF::Interval value = SVF::Interval::bottom();
        for (SVF::Location location : expression)
        {
            value.joinWith(ae->getMemoryInterval(location, node));
        }
        if (!value.isSingleton())
        {
            break;
        }
        if (static_cast<char>(value.singletonValue().toInt64()) == '\0')
        {
            break;
        }
        str0.push_back(static_cast<char>(value.singletonValue().toInt64()));
    }
    return str0;
}

void AbsExtAPI::handleExtAPI(const CallICFGNode* call)
{
    const FunObjVar* fun = call->getCalledFunction();
    assert(fun && "FunObjVar* is nullptr");
    ExtAPIType extType = UNCLASSIFIED;
    // get type of mem api
    for (const std::string& annotation :
            ExtAPI::getExtAPI()->getExtFuncAnnotations(fun))
    {
        if (annotation.find("MEMCPY") != std::string::npos)
            extType = MEMCPY;
        if (annotation.find("MEMSET") != std::string::npos)
            extType = MEMSET;
        if (annotation.find("STRCPY") != std::string::npos)
            extType = STRCPY;
        if (annotation.find("STRCAT") != std::string::npos)
            extType = STRCAT;
    }
    if (extType == UNCLASSIFIED)
    {
        if (func_map.find(fun->getName()) != func_map.end())
        {
            func_map[fun->getName()](call);
        }
        else
        {
            // The common result fallback below handles unknown functions too.
        }
    }
    // 1. memcpy functions like memcpy_chk, strncpy, annotate("MEMCPY"),
    // annotate("BUF_CHECK:Arg0, Arg2"), annotate("BUF_CHECK:Arg1, Arg2")
    else if (extType == MEMCPY)
    {
        SVF::Interval len = ae->getInterval(call->getArgument(2), call);
        handleMemcpy(call->getArgument(0), call->getArgument(1), len, 0, call);
    }
    else if (extType == MEMSET)
    {
        SVF::Interval len = ae->getInterval(call->getArgument(2), call);
        SVF::Interval elem = ae->getInterval(call->getArgument(1), call);
        handleMemset(call->getArgument(0), elem, len, call);
    }
    else if (extType == STRCPY)
    {
        handleStrcpy(call);
    }
    else if (extType == STRCAT)
    {
        // Both strcat and strncat are annotated as STRCAT.
        // Distinguish by name: strncat/wcsncat contain "ncat".
        const std::string& name = fun->getName();
        if (name.find("ncat") != std::string::npos)
            handleStrncat(call);
        else
            handleStrcat(call);
    }
    else
    {
    }

    const RetICFGNode* returnSite = call->getRetICFGNode();
    const SVFVar* rawReturn = returnSite ? returnSite->getActualRet() : nullptr;
    if (const auto* ret = rawReturn
                          ? SVFUtil::dyn_cast<ValVar>(rawReturn) : nullptr)
    {
        // Every normally returning non-void call defines its SSA result. A
        // named model may describe only side effects (for example fread) and
        // leave that result otherwise absent. Define it as initialized Top
        // within its type range, but never overwrite a precise numerical result or a pointer address
        // fact established by the handler/SVF value-flow edges.
        const bool pointerFact =
            ret->isPointer() && !ae->getAddressSet(ret, call).isBottom();
        if (!pointerFact &&
                ae->numericalValueMayBeUninitialized(ret, call))
            ae->updateInterval(ret, ae->unknownInterval(ret), call);
    }
    return;
}

// ===----------------------------------------------------------------------===//
//  Shared primitives for string/memory handlers
// ===----------------------------------------------------------------------===//

/// Get the byte size of each element for a pointer/array variable.
/// Shared by handleMemcpy, handleMemset, and getStrlen to avoid duplication.
u32_t AbsExtAPI::getElementSize(const ValVar* var)
{
    if (var->getType()->isArrayTy())
    {
        return SVFUtil::dyn_cast<SVFArrayType>(var->getType())
               ->getTypeOfElement()
               ->getByteSize();
    }
    if (var->getType()->isPointerTy())
        return 1;
    assert(false && "unsupported type for element size");
    return 1;
}

/// Check if an interval length is usable for memory operations.
/// Returns false for bottom (no information) or unbounded lower bound
/// (cannot determine a concrete start for iteration).
bool AbsExtAPI::isValidLength(const SVF::Interval& len)
{
    return !len.isBottom() && len.lower().isFinite();
}

/// Calculate the length of a null-terminated string in abstract state.
/// Scans memory from the base of strValue looking for a '\0' byte.
/// Returns an exact length when all preceding elements are definitely non-zero.
/// If an earlier element is unknown, a definite '\0' still provides a sound
/// upper bound. Returns [0, MaxFieldLimit] if no such bound is established.
SVF::Interval AbsExtAPI::getStrlen(const ValVar* strValue, const ICFGNode* node)
{
    // Step 1: determine the buffer size (in bytes) backing this pointer
    u32_t dst_size = 0;
    const SVF::AddressSet ptrVal = ae->getAddressSet(strValue, node);
    if (!ptrVal.hasUnknownObject())
        for (SVF::Location location : ptrVal)
        {
            const ObjVar* object = ae->objectAt(location);
            const BaseObjVar* baseObject =
                object ? svfir->getBaseObject(object->getId()) : nullptr;
            // Abstract addresses may denote black-hole, integer-derived, or
            // other non-object nodes.  In that case the backing size is
            // unknown; keep the conservative unknown-length result instead of
            // dereferencing a missing BaseObjVar.
            if (baseObject == nullptr)
                continue;
            if (baseObject->isConstantByteSize())
            {
                dst_size = std::max(dst_size, baseObject->getByteSizeOfObj());
            }
            else
            {
                const ICFGNode* icfgNode = baseObject->getICFGNode();
                if (icfgNode == nullptr)
                    continue;
                for (const SVFStmt* stmt2 : icfgNode->getSVFStmts())
                {
                    if (const AddrStmt* addrStmt =
                                SVFUtil::dyn_cast<AddrStmt>(stmt2))
                    {
                        dst_size = std::max(
                                       dst_size, ae->getAllocaInstByteSize(addrStmt).upperBound);
                    }
                }
            }
        }

    // Step 2: scan for a definitely positioned '\0' terminator.  A pointer
    // may denote several backing objects, so an exact result requires every
    // preceding element to be definitely non-zero across all pointees.
    // Unknown preceding elements do not invalidate a later definite
    // terminator: they only weaken the result from an exact length to an upper
    // bound because the string may terminate earlier.
    if (!ptrVal.isBottom() && ptrVal.isFinite() && dst_size != 0)
    {
        bool exactPrefix = true;
        for (u32_t index = 0; index < dst_size; index++)
        {
            const SVF::AddressSet expression =
                ae->getGepObjAddrs(strValue, integerInterval(index), node);
            if (!expression.isFinite())
                return SVF::Interval::closed(
                           SVF::Rational(0), SVF::Rational(Options::MaxFieldLimit()));
            SVF::Interval value = SVF::Interval::bottom();
            for (SVF::Location location : expression)
            {
                value.joinWith(ae->getMemoryInterval(location, node));
            }
            if (value.isZero())
            {
                const u32_t elemSize = getElementSize(strValue);
                const SVF::Rational byteLength(index * elemSize);
                return exactPrefix ? integerInterval(index * elemSize) :
                       SVF::Interval::closed(SVF::Rational(0), byteLength);
            }
            exactPrefix = exactPrefix && value.isSingleton();
        }
    }

    // No definite terminator was established.  This includes unknown backing
    // size, an empty points-to set, and a fully scanned but unterminated
    // buffer.  Preserve the documented conservative fallback.
    return SVF::Interval::closed(SVF::Rational(0),
                                 SVF::Rational(Options::MaxFieldLimit()));
}

// ===----------------------------------------------------------------------===//
//  String/memory operation handlers
// ===----------------------------------------------------------------------===//

/// strcpy(dst, src): copy all of src (including '\0') into dst.
/// Covers: strcpy, __strcpy_chk, stpcpy, wcscpy, __wcscpy_chk
void AbsExtAPI::handleStrcpy(const CallICFGNode* call)
{
    const ValVar* dst = call->getArgument(0);
    const ValVar* src = call->getArgument(1);
    SVF::Interval srcLen = getStrlen(src, call);
    if (!isValidLength(srcLen))
        return;
    handleMemcpy(dst, src, srcLen, 0, call);
}

/// strcat(dst, src): append all of src after the end of dst.
/// Covers: strcat, __strcat_chk, wcscat, __wcscat_chk
void AbsExtAPI::handleStrcat(const CallICFGNode* call)
{
    const ValVar* dst = call->getArgument(0);
    const ValVar* src = call->getArgument(1);
    SVF::Interval dstLen = getStrlen(dst, call);
    SVF::Interval srcLen = getStrlen(src, call);
    if (!isValidLength(dstLen))
        return;
    handleMemcpy(dst, src, srcLen, lowerInteger(dstLen), call);
}

/// strncat(dst, src, n): append at most n bytes of src after the end of dst.
/// Covers: strncat, __strncat_chk, wcsncat, __wcsncat_chk
void AbsExtAPI::handleStrncat(const CallICFGNode* call)
{
    const ValVar* dst = call->getArgument(0);
    const ValVar* src = call->getArgument(1);
    SVF::Interval n = ae->getInterval(call->getArgument(2), call);
    SVF::Interval dstLen = getStrlen(dst, call);
    if (!isValidLength(dstLen))
        return;
    handleMemcpy(dst, src, n, lowerInteger(dstLen), call);
}

/// Copy complete scalar cells when their byte layout is known. SVF GEP fields
/// are not byte-addressed cells: interpreting byte indices as field indices can
/// silently miss scalar writes or make a partial byte copy a full assignment.
/// Unsupported layouts conservatively lose the affected destination contents.
void AbsExtAPI::handleMemcpy(const ValVar* dst, const ValVar* src,
                             const SVF::Interval& len, u32_t start_idx,
                             const ICFGNode* node)
{
    const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node);
    const FunObjVar* callee = call ? call->getCalledFunction() : nullptr;
    const std::string name = callee ? callee->getName() : "";
    // MEMCPY annotations also include strncpy and memccpy, with stopping,
    // padding or a different length argument. They require the havoc fallback.
    const bool exactByteCopy = name == "memcpy" || name == "memmove" ||
                               name == "__memcpy_chk" || name == "__memmove_chk" ||
                               name.rfind("llvm.memcpy.", 0) == 0 || name.rfind("llvm.memmove.", 0) == 0 ||
                               name.rfind("llvm_memcpy", 0) == 0 || name.rfind("llvm_memmove", 0) == 0;
    if (exactByteCopy && len.isZero())
        return;
    const SVF::AddressSet sources = ae->getAddressSet(src, node);
    const SVF::AddressSet destinations = ae->getAddressSet(dst, node);
    if (exactByteCopy && start_idx == 0 && len.isSingleton() && sources.isSingleton() &&
            destinations.isSingleton())
    {
        const SVF::Location source = *sources.begin();
        const SVF::Location destination = *destinations.begin();
        const ObjVar* sourceCell = source.isNull() ? nullptr : ae->objectAt(source);
        const ObjVar* destinationCell = destination.isNull() ? nullptr : ae->objectAt(destination);
        const auto* sourceObject = sourceCell ? SVFUtil::dyn_cast<BaseObjVar>(sourceCell) : nullptr;
        const auto* destinationObject = destinationCell ? SVFUtil::dyn_cast<BaseObjVar>(destinationCell) : nullptr;
        // Array elements are field cells, not bytes. Copy only complete,
        // identically typed scalar elements; preserve source snapshots before
        // any write so memmove overlap cannot read an already changed cell.
        const auto* sourceArray = sourceObject
                                  ? SVFUtil::dyn_cast<SVFArrayType>(sourceObject->getType()) : nullptr;
        const auto* destinationArray = destinationObject
                                       ? SVFUtil::dyn_cast<SVFArrayType>(destinationObject->getType()) : nullptr;
        if (sourceArray && destinationArray && sourceArray == destinationArray &&
                !sourceArray->getTypeOfElement()->isArrayTy() &&
                !sourceArray->getTypeOfElement()->isStructTy() &&
                sourceObject->isConstantByteSize() && destinationObject->isConstantByteSize())
        {
            const u32_t elementBytes = sourceArray->getTypeOfElement()->getByteSize();
            const s64_t bytes = lowerInteger(len);
            if (elementBytes && bytes > 0 && bytes % elementBytes == 0 &&
                    static_cast<u64_t>(bytes) <= sourceObject->getByteSizeOfObj() &&
                    static_cast<u64_t>(bytes) <= destinationObject->getByteSizeOfObj() &&
                    static_cast<u64_t>(bytes / elementBytes) <= Options::MaxFieldLimit())
            {
                struct CellCopy
                {
                    SVF::Location destination;
                    SVF::Interval interval;
                    SVF::AddressSet addresses;
                };
                std::vector<CellCopy> copies;
                bool complete = true;
                for (s64_t index = 0; index < bytes / elementBytes; ++index)
                {
                    const SVF::AddressSet from = ae->getGepObjAddrs(src, integerInterval(index), node);
                    const SVF::AddressSet to = ae->getGepObjAddrs(dst, integerInterval(index), node);
                    if (!from.isSingleton() || !to.isSingleton() || from.begin()->isNull() || to.begin()->isNull())
                    {
                        complete = false;
                        break;
                    }
                    const ObjVar* target = ae->objectAt(*to.begin());
                    if (!target || !ae->canStrongStore(to, *target, ae->state(node)) ||
                            !ae->hasMemoryValue(*from.begin(), node))
                    {
                        complete = false;
                        break;
                    }
                    copies.push_back({*to.begin(), ae->getMemoryInterval(*from.begin(), node),
                                      ae->getMemoryAddressSet(*from.begin(), node)});
                }
                if (complete)
                {
                    for (const auto& copy : copies)
                        ae->updateMemoryValue(copy.destination, copy.interval, copy.addresses, node);
                    return;
                }
            }
        }
        if (sourceObject && destinationObject &&
                !sourceObject->isArray() && !sourceObject->isStruct() &&
                !destinationObject->isArray() && !destinationObject->isStruct() &&
                sourceObject->getType() == destinationObject->getType() &&
                sourceObject->isConstantByteSize() &&
                destinationObject->isConstantByteSize() &&
                sourceObject->getByteSizeOfObj() > 0 &&
                sourceObject->getByteSizeOfObj() == destinationObject->getByteSizeOfObj() &&
                len.singletonValue() == SVF::Rational(sourceObject->getByteSizeOfObj()) &&
                ae->canStrongStore(destinations, *destinationObject, ae->state(node)) &&
                ae->hasMemoryValue(source, node))
        {
            const SVF::Interval value = ae->getMemoryInterval(source, node);
            const SVF::AddressSet addresses = ae->getMemoryAddressSet(source, node);
            ae->updateMemoryValue(destination, value, addresses, node);
            return;
        }
    }

    // Havoc every represented field of every possible destination allocation.
    // Missing/unknown destinations cannot justify retaining any memory fact.
    bool all = destinations.hasUnknownObject() || destinations.mayContainRawAddress() ||
               destinations.isBottom();
    std::set<NodeID> bases;
    // The all-target fallback already covers unknown destinations. Object
    // Top has no finite enumeration, so only inspect the finite case here.
    if (!all)
    {
        for (SVF::Location location : destinations.locations())
        {
            const ObjVar* object = location.isNull() ? nullptr : ae->objectAt(location);
            const BaseObjVar* base = object ? svfir->getBaseObject(object->getId()) : nullptr;
            if (!base || base->isBlackHoleObj())
                all = true;
            else
                bases.insert(base->getId());
        }
    }
    // A later GEP must not discover an apparently uninitialized field after
    // this allocation has already been overwritten. Materialize the finite
    // canonical field universe before havoc, including currently unused cells.
    if (!all && Options::AEDomain() == AbstractInterpretation::AENumericalDomain::Box)
        for (NodeID baseId : bases)
        {
            const BaseObjVar* base = svfir->getBaseObject(baseId);
            const u32_t limit = std::max<u32_t>(1, base->getMaxFieldOffsetLimit());
            for (u32_t index = 0; index < limit; ++index)
            {
                const NodeID field = svfir->getGepObjVar(baseId, index);
                ae->locationOf(SVFUtil::cast<ObjVar>(svfir->getSVFVar(field)));
            }
        }
    std::vector<SVF::Location> targets;
    for (const auto& [location, content] : ae->state(node).memoryLayout().cells())
    {
        (void)content;
        const ObjVar* object = ae->objectAt(location);
        const BaseObjVar* base = object ? svfir->getBaseObject(object->getId()) : nullptr;
        if (all || (base && bases.count(base->getId())))
            targets.push_back(location);
    }
    for (SVF::Location location : targets)
        ae->updateMemoryValue(location, SVF::Interval::top(), SVF::AddressSet::top(), node);
}

/// Core memset: fill dst with `elem` for `len` bytes.
/// Note: elemSize here uses the pointee type's full size (not array element
/// size) to match how LLVM memset/wmemset intrinsics measure `len`. For a
/// pointer to wchar_t[100], elemSize = sizeof(wchar_t[100]), so range_val
/// reflects the number of top-level GEP fields, not individual array elements.
void AbsExtAPI::handleMemset(const ValVar* dst, const SVF::Interval& elem,
                             const SVF::Interval& len, const ICFGNode* node)
{
    if (!isValidLength(len) || len.lower().value() <= SVF::Rational(0))
        return;

    u32_t elemSize = 1;
    if (dst->getType()->isArrayTy())
    {
        elemSize = SVFUtil::dyn_cast<SVFArrayType>(dst->getType())
                   ->getTypeOfElement()
                   ->getByteSize();
    }
    else if (dst->getType()->isPointerTy())
    {
        elemSize = 1;
    }
    else
    {
        assert(false && "unsupported type for element size");
    }
    u32_t size = std::min((u32_t)Options::MaxFieldLimit(),
                          static_cast<u32_t>(lowerInteger(len)));
    u32_t range_val = size / elemSize;

    for (u32_t index = 0; index < range_val; index++)
    {
        if (ae->getAddressSet(dst, node).isBottom())
            break;
        const SVF::AddressSet locations =
            ae->getGepObjAddrs(dst, integerInterval(index), node);
        if (!locations.isFinite())
            break;
        for (SVF::Location location : locations)
        {
            if (ae->hasMemoryValue(location, node))
            {
                SVF::Interval value = ae->getMemoryInterval(location, node);
                value.joinWith(elem);
                ae->updateMemoryValue(location, value,
                                      ae->getMemoryAddressSet(location, node),
                                      node);
            }
            else
            {
                ae->updateMemoryValue(location, elem, SVF::AddressSet::bottom(),
                                      node);
            }
        }
    }
}

/**
 * This function, getRangeLimitFromType, calculates the lower and upper bounds
 * of a numeric range for a given SVFType. It is used to determine the possible
 * value range of integer types. If the type is an SVFIntegerType, it calculates
 * the bounds based on the size and signedness of the type. The calculated
 * bounds are returned as an interval representing the lower and upper limits of
 * the range.
 *
 * @param type   The SVFType for which to calculate the value range.
 *
 * @return       An interval representing the lower and upper bounds.
 */
SVF::Interval AbsExtAPI::getRangeLimitFromType(const SVFType* type)
{
    if (const SVFIntegerType* intType = SVFUtil::dyn_cast<SVFIntegerType>(type))
    {
        const u32_t bits = type->getByteSize() * 8;
        return bits == 0 ? SVF::Interval::top()
               : SVF::integerRange(bits, intType->isSigned());
    }
    else if (SVFUtil::isa<SVFOtherType>(type))
    {
        // handle other type like float double, set s32_t as the range
        s64_t ub = static_cast<s64_t>(std::numeric_limits<s32_t>::max());
        s64_t lb = static_cast<s64_t>(std::numeric_limits<s32_t>::min());
        return SVF::Interval::closed(SVF::Rational(lb), SVF::Rational(ub));
    }
    else
    {
        return SVF::Interval::top();
        // other types, return top interval
    }
}
