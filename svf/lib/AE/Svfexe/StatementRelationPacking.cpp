// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AE/Svfexe/StatementRelationPacking.h"
#include "AE/Svfexe/SVFIRAdapter.h"
#include "AE/Svfexe/AllocationSize.h"
#include "MemoryModel/PointerAnalysis.h"
#include "SVFIR/SVFIR.h"
#include <algorithm>
#include <optional>
#include <set>

namespace SVF
{
namespace AD = AbstractDomain;

StatementPackingResult StatementRelationPacking::pack(
    const std::vector<AD::Variable>& vocabulary) const
{
    const std::set<AD::Variable> permitted(vocabulary.begin(), vocabulary.end());
    StatementPackingResult result;
    std::vector<AD::RelationEdge> edges;
    const auto numeric = [&](const SVFVar* variable) -> std::optional<AD::Variable>
    {
        const auto* value = SVFUtil::dyn_cast<ValVar>(variable);
        if (!value || value->isPointer() || !adapter_.contains(*value))
            return std::nullopt;
        const auto mapped = adapter_.variable(*value);
        if (mapped.type().kind == AD::NumericKind::IEEEFloat ||
                permitted.count(mapped) == 0)
            return std::nullopt;
        return mapped;
    };
    const auto add = [&](std::optional<AD::Variable> left,
                         std::optional<AD::Variable> right,
                         const SVFStmt* statement, const char* kind)
    {
        if (!left || !right || *left == *right)
            return;
        AD::RelationEdge edge{std::min(*left, *right), std::max(*left, *right)};
        edges.push_back(edge);
        result.edges.push_back({edge, statement->getEdgeID(), kind});
    };
    const auto contentEdges = [&](const ValVar* pointer,
                                  const SVFVar* value,
                                  const SVFStmt* statement, const char* kind)
    {
        const auto scalar = numeric(value);
        if (!scalar)
            return;
        for (NodeID id : pointsTo_.getPts(pointer->getId()))
        {
            const auto* object = SVFUtil::dyn_cast<ObjVar>(ir_.getGNode(id));
            if (!object || object->isPointer())
                continue;
            const auto content = adapter_.contentVariable(*object);
            if (content.type().kind != AD::NumericKind::IEEEFloat &&
                    permitted.count(content) != 0)
                add(scalar, content, statement, kind);
        }
    };
    std::vector<const SVFStmt*> statements;
    for (auto kind : {SVFStmt::BinaryOp, SVFStmt::UnaryOp, SVFStmt::Cmp,
                      SVFStmt::Copy, SVFStmt::Phi, SVFStmt::Select,
                      SVFStmt::Call, SVFStmt::Ret, SVFStmt::Load, SVFStmt::Store})
    {
        const auto& set = ir_.getSVFStmtSet(kind);
        statements.insert(statements.end(), set.begin(), set.end());
    }
    std::sort(statements.begin(), statements.end(),
              [](const SVFStmt* a, const SVFStmt* b)
    {
        return a->getEdgeID() < b->getEdgeID();
    });
    for (const auto* statement : statements)
    {
        if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement))
            contentEdges(load->getRHSVar(), load->getLHSVar(), statement, "load");
        else if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(statement))
            contentEdges(store->getLHSVar(), store->getRHSVar(), statement, "store");
        else if (const auto* cmp = SVFUtil::dyn_cast<CmpStmt>(statement))
            // The Boolean result is not an affine partner of either operand.
            add(numeric(cmp->getOpVar(0)), numeric(cmp->getOpVar(1)), statement, "cmp");
        else if (const auto* unary = SVFUtil::dyn_cast<UnaryOPStmt>(statement))
            add(numeric(unary->getRes()), numeric(unary->getOpVar()), statement, "unary");
        else if (const auto* multi = SVFUtil::dyn_cast<MultiOpndStmt>(statement))
        {
            const char* kind = SVFUtil::isa<BinaryOPStmt>(statement) ? "binary" :
                               SVFUtil::isa<PhiStmt>(statement) ? "phi" :
                               SVFUtil::isa<SelectStmt>(statement) ? "select" : "call";
            for (const auto* operand : multi->getOpndVars())
                add(numeric(multi->getRes()), numeric(operand), statement, kind);
        }
        else if (const auto* assign = SVFUtil::dyn_cast<AssignStmt>(statement))
            add(numeric(assign->getLHSVar()), numeric(assign->getRHSVar()), statement,
                SVFUtil::isa<RetPE>(statement) ? "ret" : "copy");
    }
    if (sizes_)
    {
        // D-005: Size(o) := allocated count, and every variable array index
        // is compared against the Size(o) of its possible base objects.
        const auto sizeOf = [&](const BaseObjVar* object) -> std::optional<AD::Variable>
        {
            const AllocationSize* size = object ? sizes_->find(object) : nullptr;
            if (!size)
                return std::nullopt;
            const auto variable = adapter_.sizeVariable(*size->object);
            return permitted.count(variable) ? std::optional<AD::Variable>(variable)
                   : std::nullopt;
        };
        for (const auto& size : sizes_->all())
            if (size.sizeValue)
                add(sizeOf(size.object), numeric(size.operand ? size.operand : size.sizeValue),
                    size.site, "size");
        for (const SVFStmt* statement : ir_.getSVFStmtSet(SVFStmt::Gep))
        {
            const auto* gep = SVFUtil::cast<GepStmt>(statement);
            if (gep->isConstantOffset())
                continue;
            std::set<const BaseObjVar*> bases;
            for (NodeID id : pointsTo_.getPts(gep->getRHSVarID()))
                if (const auto* base = ir_.getBaseObject(id))
                    bases.insert(base);
            for (const auto& part : gep->getOffsetVarAndGepTypePairVec())
                if (!SVFUtil::isa<ConstIntValVar>(part.first))
                    for (const auto* base : bases)
                        add(numeric(part.first), sizeOf(base), statement, "index");
        }
        for (auto it = ir_.getICFG()->begin(); it != ir_.getICFG()->end(); ++it)
        {
            const auto* call = SVFUtil::dyn_cast<CallICFGNode>(it->second);
            const auto* function = call ? call->getCalledFunction() : nullptr;
            if (!function || call->arg_size() < 3)
                continue;
            const auto& name = function->getName();
            if (name != "memcpy" && name != "memmove" && name != "strncpy" &&
                    name != "__memcpy_chk" && name != "__memmove_chk" &&
                    name.rfind("llvm.memcpy.", 0) != 0 && name.rfind("llvm.memmove.", 0) != 0 &&
                    name.rfind("llvm_memcpy", 0) != 0 && name.rfind("llvm_memmove", 0) != 0)
                continue;
            for (unsigned argument : {0u, 1u})
                for (NodeID id : pointsTo_.getPts(call->getArgument(argument)->getId()))
                    if (const auto* base = ir_.getBaseObject(id))
                        if (const auto* size = sizes_->find(base))
                            add(numeric(call->getArgument(2)), sizeOf(base), size->site, "length");
        }
    }
    result.packs = AD::connectedComponentPacking(vocabulary, edges);
    return result;
}
}
