// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AE/Svfexe/AllocationSize.h"
#include "SVFIR/SVFIR.h"
#include "Util/ExtAPI.h"
#include <sstream>
#include <string>

namespace SVF
{
namespace
{
template <typename T, typename U>
const T* orNull(const U* value)
{
    return value ? SVFUtil::dyn_cast<T>(value) : nullptr;
}

const SVFStmt* definition(const SVFVar* value)
{
    const auto* variable = SVFUtil::dyn_cast<ValVar>(value);
    if (!variable || variable->getInEdges().size() != 1)
        return nullptr;
    return *variable->getInEdges().begin();
}

bool integerType(const SVFVar* value)
{
    return value && SVFUtil::isa<SVFIntegerType>(value->getType());
}

s64_t arrayLength(const SVFArrayType* array)
{
    const SVFType* element = array ? array->getTypeOfElement() : nullptr;
    return element && element->getByteSize()
           ? static_cast<s64_t>(array->getByteSize() / element->getByteSize()) : 0;
}

const SVFVar* argument(const CallICFGNode* call, u32_t index)
{
    return call && index < call->arg_size() && integerType(call->getArgument(index))
           ? call->getArgument(index) : nullptr;
}

bool contains(const std::string& name, const char* part)
{
    return name.find(part) != std::string::npos;
}
} // namespace

namespace
{
const SVFVar* castSource(const SVFVar* value)
{
    const auto* copy = orNull<CopyStmt>(definition(value));
    if (!copy || !integerType(copy->getRHSVar()))
        return nullptr;
    const auto kind = copy->getCopyKind();
    return kind == CopyStmt::COPYVAL || kind == CopyStmt::ZEXT ||
           kind == CopyStmt::SEXT || kind == CopyStmt::TRUNC
           ? copy->getRHSVar() : nullptr;
}
} // namespace

const SVFVar* AllocationSizeModel::stripCasts(const SVFVar* value)
{
    while (const SVFVar* source = castSource(value))
        value = source;
    return value;
}

const SVFVar* AllocationSizeModel::stripCoefficient(const SVFVar* value,
        s64_t& coefficient, std::vector<const SVFVar*>* chain)
{
    coefficient = 1;
    for (;;)
    {
        for (const SVFVar* source = value; source; source = castSource(source))
        {
            if (chain)
                chain->push_back(source);
            value = source;
        }
        const auto* binary = orNull<BinaryOPStmt>(definition(value));
        if (!binary)
            return value;
        const auto* lhs = binary->getOpVar(0);
        const auto* rhs = binary->getOpVar(1);
        const auto* lhsConstant = SVFUtil::dyn_cast<ConstIntValVar>(lhs);
        const auto* rhsConstant = SVFUtil::dyn_cast<ConstIntValVar>(rhs);
        if (binary->getOpcode() == BinaryOPStmt::Mul && (lhsConstant || rhsConstant) &&
                !(lhsConstant && rhsConstant))
        {
            const s64_t factor = lhsConstant ? lhsConstant->getSExtValue()
                                 : rhsConstant->getSExtValue();
            if (factor <= 0)
                return value;
            coefficient *= factor;
            value = lhsConstant ? rhs : lhs;
        }
        else if (binary->getOpcode() == BinaryOPStmt::Shl && rhsConstant && !lhsConstant &&
                 rhsConstant->getSExtValue() >= 0 && rhsConstant->getSExtValue() < 31)
        {
            coefficient *= s64_t(1) << rhsConstant->getSExtValue();
            value = lhs;
        }
        else
            return value;
    }
}

const CallICFGNode* AllocationSizeModel::allocationCall(const AddrStmt* site)
{
    const ICFGNode* node = site ? site->getICFGNode() : nullptr;
    if (const auto* call = orNull<CallICFGNode>(node))
        return call;
    if (const auto* ret = orNull<RetICFGNode>(node))
        return ret->getCallICFGNode();
    return nullptr;
}

AllocationSizeModel::AllocationSizeModel(SVFIR& ir)
{
    for (const SVFStmt* statement : ir.getSVFStmtSet(SVFStmt::Addr))
    {
        const auto* site = SVFUtil::cast<AddrStmt>(statement);
        const auto* object = SVFUtil::dyn_cast<BaseObjVar>(site->getRHSVar());
        if (!object || object->isFunction() || object->isBlackHoleObj() ||
                SVFUtil::isa<ConstDataObjVar>(object) || SVFUtil::isa<DummyObjVar>(object) ||
                byObject_.count(object))
            continue;
        AllocationSize size;
        size.object = object;
        size.site = site;
        const SVFType* type = object->getType();
        const auto* array = orNull<SVFArrayType>(type);
        const auto setOperand = [&](const SVFVar* value, s64_t unit)
        {
            if (!value) return;
            size.sizeValue = value;
            s64_t coefficient = 1;
            const SVFVar* stripped = stripCoefficient(value, coefficient);
            size.strippedCoefficient = coefficient;
            size.unitBytes = static_cast<u32_t>(unit);
            size.elementBytes = static_cast<u32_t>(unit * coefficient);
            size.known = true;
            if (const auto* constant = SVFUtil::dyn_cast<ConstIntValVar>(stripped))
                size.constant = constant->getSExtValue();
            else
                size.operand = SVFUtil::dyn_cast<ValVar>(stripped);
            if (!size.operand && !SVFUtil::isa<ConstIntValVar>(stripped))
                size.known = false;
        };
        if (SVFUtil::isa<HeapObjVar>(object))
        {
            // Sparrow models malloc as Calloc(Array e), calloc/realloc as
            // library calls setting Size, and strdup as a Size copy. SVF's
            // ExtAPI "AllocSize:ArgK[*ArgL]" annotation names the operands.
            const CallICFGNode* call = allocationCall(site);
            const FunObjVar* callee = call ? call->getCalledFunction() : nullptr;
            const std::string name = callee ? callee->getName() : "";
            std::vector<u32_t> operands;
            if (callee)
                for (const std::string& annotation :
                        ExtAPI::getExtAPI()->getExtFuncAnnotations(callee))
                {
                    const auto at = annotation.find("AllocSize:");
                    if (at == std::string::npos)
                        continue;
                    std::stringstream terms(annotation.substr(at + 10));
                    std::string term;
                    while (std::getline(terms, term, '*'))
                        if (term.rfind("Arg", 0) == 0)
                            operands.push_back(static_cast<u32_t>(std::stoul(term.substr(3))));
                }
            if (operands.size() == 1)
                setOperand(argument(call, operands[0]), 1);
            else if (operands.size() == 2)
            {
                // calloc(n, s): Sparrow sets Size := n; s is the element size.
                setOperand(argument(call, operands[0]), 1);
                const auto* element = orNull<ConstIntValVar>(argument(call, operands[1]));
                size.elementBytes = element && element->getSExtValue() > 0
                                    ? static_cast<u32_t>(size.strippedCoefficient *
                                                         element->getSExtValue())
                                    : 0;
                size.unitBytes = 0;
            }
            else if (contains(name, "strdup"))
                size.sizeSourcePointer = call->arg_size() ? call->getArgument(0) : nullptr;
        }
        else if (SVFUtil::isa<StackObjVar>(object))
        {
            const u32_t unit = array && array->getTypeOfElement()
                               ? array->getTypeOfElement()->getByteSize()
                               : (type ? type->getByteSize() : 0);
            const auto& counts = site->getArrSize();
            const SVFVar* count = counts.size() == 1 ? counts.front() : nullptr;
            const auto* constantCount = orNull<ConstIntValVar>(count);
            if (count && !constantCount)
            {
                setOperand(count, unit);
                if (array)
                    size.known = false; // alloca [N x T], %n: not modelled
            }
            else if (array || (constantCount && constantCount->getSExtValue() > 1))
            {
                size.known = true;
                size.elementBytes = unit;
                size.constant = (constantCount ? constantCount->getSExtValue() : 1) *
                                (array ? arrayLength(array) : 1);
            }
            else
                continue;
        }
        else if (SVFUtil::isa<GlobalObjVar>(object) && array)
        {
            size.known = true;
            size.elementBytes = array->getTypeOfElement()
                                ? array->getTypeOfElement()->getByteSize() : 0;
            size.constant = arrayLength(array);
        }
        else
            continue;
        byObject_[object] = sizes_.size();
        bySite_[site] = sizes_.size();
        sizes_.push_back(size);
    }
}

const AllocationSize* AllocationSizeModel::find(const BaseObjVar* object) const
{
    const auto found = byObject_.find(object);
    return found == byObject_.end() ? nullptr : &sizes_[found->second];
}

const AllocationSize* AllocationSizeModel::find(const AddrStmt* site) const
{
    const auto found = bySite_.find(site);
    return found == bySite_.end() ? nullptr : &sizes_[found->second];
}
} // namespace SVF
