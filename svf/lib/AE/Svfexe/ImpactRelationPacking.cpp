// SPDX-License-Identifier: AGPL-3.0-or-later
// Port of ROPAS Sparrow octImpactSem.ml / octImpactAnalysis.ml.
// Each rule names its Sparrow counterpart; "SVF:" marks LLVM/SVF adaptations.
// Upstream: https://github.com/ropas/sparrow
// Revision: 3ec055b8c87b5c8340ef3ed6cde34f5835865b31
// Sparrow-derived portions retain the following upstream notice:
// BSD 3-Clause License
//
// Copyright (c) 2007-present,
// Programming Research Laboratory (ROPAS), Seoul National University, Korea
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "AE/Svfexe/ImpactRelationPacking.h"
#include "AE/Core/ImpactPagedGraph.h"
#include "AE/Svfexe/AllocationSize.h"
#include "AE/Svfexe/SVFIRAdapter.h"
#include "Graphs/CallGraph.h"
#include "MemoryModel/PointerAnalysis.h"
#include "SVFIR/SVFIR.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <functional>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace SVF
{
namespace
{
using Var = SVF::Variable;
using Index = SVF::ImpactPagedGraph::Index;

std::map<NodeID, std::uintptr_t>& sourceVariableMap()
{
    static std::map<NodeID, std::uintptr_t> map;
    return map;
}

/// A Sparrow PowOctLoc restricted to V. `strong` is can_strong_update_octloc
/// of the whole location set (exactly one location that allows it).
struct Locs
{
    std::vector<Index> vars;
    bool strong = false;
    const SVFVar* value = nullptr;  // the SSA location, when it is one
    std::set<NodeID> objects;       // memory locations, when they are
};

struct Command
{
    enum Kind { SetConst, SetVariable, Forget, Assume, Alternatives } kind = Forget;
    std::vector<Index> lhs, rhs;
    bool strong = false;     // both location sets allow a strong update
    bool weakUpdate = false; // Sparrow update -> Mem.weak_add
    std::vector<std::vector<Command>> alternatives;
};
using Commands = std::vector<Command>;

template <class G>
void apply(const Command& command, G& graph)
{
    if (graph.bottom)
        return;
    if (command.weakUpdate)
    {
        // octImpactSem.update: in a recursive procedure (or a Weak binding)
        // the new value is joined into the old one.
        Command strongUpdate = command;
        strongUpdate.weakUpdate = false;
        G updated = graph;
        apply(strongUpdate, updated);
        graph = graph.join(updated);
        return;
    }
    switch (command.kind)
    {
    case Command::SetConst:
        for (Index x : command.lhs)
            graph.setConstant(x, command.strong);
        break;
    case Command::SetVariable:
        // PowOctLoc.fold2 AbsOct.(weak_)set_variable lv_set rv_set
        for (Index x : command.lhs)
            for (Index y : command.rhs)
                graph.setVariable(x, y, command.strong);
        break;
    case Command::Forget:
        for (Index x : command.lhs)
            graph.forget(x);
        break;
    case Command::Assume:
        // weak_assume is the identity.
        if (command.strong)
            for (Index x : command.lhs)
                for (Index y : command.rhs)
                    graph.assume(x, y);
        break;
    case Command::Alternatives:
    {
        G result;
        for (const auto& alternative : command.alternatives)
        {
            G branch = graph;
            for (const auto& step : alternative)
                apply(step, branch);
            result = result.join(branch);
        }
        graph = std::move(result);
        break;
    }
    }
}

template <class G>
void applyAll(const Commands& commands, G& graph)
{
    for (const auto& command : commands)
        apply(command, graph);
}

/// The CIL expression a Sparrow command would see for an LLVM value, after
/// CilHelper.remove_cast and remove_coeff. SSA temporaries of casts,
/// constant multiplications and +/- constants are looked through.
struct Expr
{
    enum Kind { NonIntegral, Const, Var, VarPlusConst, AddVars, SubVars, Complex } kind = Complex;
    const SVFVar* var = nullptr;
    const SVFVar* var2 = nullptr;
    bool plusOne = false;
};

enum class Op { Lt, Le, Gt, Ge, Eq, Ne };
Op reverse(Op op)
{
    switch (op)
    {
    case Op::Lt:
        return Op::Gt;
    case Op::Gt:
        return Op::Lt;
    case Op::Le:
        return Op::Ge;
    case Op::Ge:
        return Op::Le;
    default:
        return op;
    }
}
Op negate(Op op)
{
    switch (op)
    {
    case Op::Lt:
        return Op::Ge;
    case Op::Gt:
        return Op::Le;
    case Op::Le:
        return Op::Gt;
    case Op::Ge:
        return Op::Lt;
    case Op::Eq:
        return Op::Ne;
    default:
        return Op::Eq;
    }
}
std::optional<Op> comparison(u32_t predicate)
{
    // SVF: CIL comparisons are untyped; signed and unsigned map alike.
    switch (predicate)
    {
    case CmpStmt::ICMP_SLT:
    case CmpStmt::ICMP_ULT:
        return Op::Lt;
    case CmpStmt::ICMP_SLE:
    case CmpStmt::ICMP_ULE:
        return Op::Le;
    case CmpStmt::ICMP_SGT:
    case CmpStmt::ICMP_UGT:
        return Op::Gt;
    case CmpStmt::ICMP_SGE:
    case CmpStmt::ICMP_UGE:
        return Op::Ge;
    case CmpStmt::ICMP_EQ:
        return Op::Eq;
    case CmpStmt::ICMP_NE:
        return Op::Ne;
    default:
        return std::nullopt;
    }
}

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

bool integral(const SVFVar* value)
{
    return value && value->getType() && SVFUtil::isa<SVFIntegerType>(value->getType());
}

bool integerCast(const CopyStmt* copy)
{
    const auto kind = copy->getCopyKind();
    return (kind == CopyStmt::COPYVAL || kind == CopyStmt::ZEXT ||
            kind == CopyStmt::SEXT || kind == CopyStmt::TRUNC) &&
           integral(copy->getRHSVar());
}

bool isMemcpyLike(const std::string& name)
{
    // AlarmExp: Strncpy, Memcpy, Memmove (LLVM intrinsics carry a suffix).
    return name == "memcpy" || name == "memmove" || name == "strncpy" ||
           name == "__memcpy_chk" || name == "__memmove_chk" ||
           name.rfind("llvm_memcpy", 0) == 0 || name.rfind("llvm_memmove", 0) == 0 ||
           name.rfind("llvm.memcpy.", 0) == 0 || name.rfind("llvm.memmove.", 0) == 0;
}

/// Program-wide reverse postorder over the ICFG; heads are targets of
/// retreating edges (Sparrow's Worklist orders a WTO of the DUG).
struct Order
{
    std::vector<unsigned> rank;
    std::vector<bool> head;
};
} // namespace

void ImpactRelationPacking::setSourceVariables(std::map<NodeID, std::uintptr_t> sourceVariables)
{
    sourceVariableMap() = std::move(sourceVariables);
}

ImpactRelationPacking::ImpactRelationPacking(SVFIR& ir, const SVFIRAdapter& adapter,
        PointerAnalysis& pointsTo, std::vector<const ICFGNode*> entries,
        const AllocationSizeModel* sizes)
    : ir_(ir), adapter_(adapter), pointsTo_(pointsTo), entries_(std::move(entries)),
      sizes_(sizes)
{
    if (!sizes_)
    {
        ownedSizes_ = std::make_unique<AllocationSizeModel>(ir_);
        sizes_ = ownedSizes_.get();
    }
}

ImpactRelationPacking::~ImpactRelationPacking() = default;

ImpactPackingResult ImpactRelationPacking::pack(const std::vector<Var>& vocabulary) const
{
    if (std::getenv("SVF_FORBID_IMPACT"))
        throw std::logic_error("impact execution forbidden in learned inference");
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const std::set<Var> permitted(vocabulary.begin(), vocabulary.end());
    // Dense coordinates for the paged graph; universe maps them back to V.
    const std::vector<Var> universe(permitted.begin(), permitted.end());
    std::unordered_map<std::uint32_t, Index> coordinates;
    for (Index i = 0; i < universe.size(); ++i)
        coordinates.emplace(universe[i].id(), i);
    const auto coordinate = [&](Var symbol) -> std::optional<Index>
    {
        const auto found = coordinates.find(symbol.id());
        if (found == coordinates.end() || universe[found->second] != symbol)
            return std::nullopt;
        return found->second;
    };
    const bool diagnostics = std::getenv("SVF_IMPACT_DIAGNOSTICS") != nullptr;
    const bool assertQueries = std::getenv("SVF_IMPACT_ASSERT_QUERIES") != nullptr;
    ImpactPackingResult result;
    ICFG* icfg = ir_.getICFG();

    // ---- location helpers (Sparrow eval_lv + can_strong_update_octloc) ----
    std::unordered_map<const FunObjVar*, bool> recursiveCache;
    const auto recursive = [&](const FunObjVar* function)
    {
        // Global.is_rec; unknown procedures are treated as non-recursive.
        if (!function || !pointsTo_.getCallGraphSCC() || !pointsTo_.getCallGraph() ||
                !pointsTo_.getCallGraph()->getCallGraphNode(function))
            return false;
        const auto found = recursiveCache.find(function);
        if (found != recursiveCache.end())
            return found->second;
        const bool value = pointsTo_.isInRecursion(function);
        recursiveCache.emplace(function, value);
        return value;
    };
    const auto numeric = [&](const SVFVar* variable) -> std::optional<Index>
    {
        const auto* value = SVFUtil::dyn_cast<ValVar>(variable);
        if (!value || value->isPointer() || !adapter_.contains(*value))
            return std::nullopt;
        return coordinate(adapter_.variable(*value));
    };
    const auto valueLocs = [&](const SVFVar* value)
    {
        // An SSA value is an LVar of its function: strong unless recursive.
        Locs locs;
        locs.value = value;
        if (const auto symbol = numeric(value))
            locs.vars.push_back(*symbol);
        locs.strong = !recursive(value->getFunction());
        return locs;
    };
    const auto pointees = [&](const SVFVar* pointer)
    {
        std::vector<const ObjVar*> objects;
        for (NodeID id : pointsTo_.getPts(pointer->getId()))
        {
            const auto* object = SVFUtil::dyn_cast<ObjVar>(ir_.getGNode(id));
            const BaseObjVar* base = object ? ir_.getBaseObject(id) : nullptr;
            if (!object || !base || base->isBlackHoleObj() ||
                    SVFUtil::isa<ConstNullPtrObjVar>(base) || id == ir_.getNullPtr())
                continue;
            objects.push_back(object);
        }
        return objects;
    };
    const auto strongObject = [&](const ObjVar* object)
    {
        // Only Loc of a GVar, or of an LVar of a non-recursive procedure.
        // Fields, arrays (allocsites), heap and summaries are weak.
        const auto* base = SVFUtil::dyn_cast<BaseObjVar>(object);
        if (!base || base->isHeap() || base->isFieldInsensitive() ||
                SVFUtil::isa<HeapObjVar>(base) || SVFUtil::isa<ConstDataObjVar>(base))
            return false;
        const SVFType* type = base->getType();
        if (type && (SVFUtil::isa<SVFStructType>(type) || SVFUtil::isa<SVFArrayType>(type)))
            return false;
        if (SVFUtil::isa<GlobalObjVar>(base))
            return true;
        return SVFUtil::isa<StackObjVar>(base) && !recursive(base->getFunction());
    };
    const auto contentLocs = [&](const SVFVar* pointer)
    {
        Locs locs;
        const auto objects = pointees(pointer);
        locs.strong = objects.size() == 1 && strongObject(objects.front());
        for (const auto* object : objects)
        {
            locs.objects.insert(object->getId());
            if (object->isPointer())
                continue;
            if (const auto symbol = coordinate(adapter_.contentVariable(*object)))
                locs.vars.push_back(*symbol);
        }
        return locs;
    };
    const auto sizeOf = [&](const BaseObjVar* base) -> std::optional<Index>
    {
        if (!base || !sizes_->find(base))
        return std::nullopt;
        return coordinate(adapter_.sizeVariable(*base));
    };
const auto sizeLocsOfPointer = [&](const SVFVar* pointer)
    {
        // ItvDom.Val.allocsites_of_val |> PowOctLoc.of_sizes
        Locs locs;
        std::set<const BaseObjVar*> bases;
        for (const auto* object : pointees(pointer))
            bases.insert(ir_.getBaseObject(object->getId()));
        std::size_t count = 0;
        for (const auto* base : bases)
            if (sizes_->find(base))
            {
                ++count;
                if (const auto symbol = sizeOf(base))
                    locs.vars.push_back(*symbol);
            }
        locs.strong = count == 1;
        return locs;
    };
    const auto sourceVariable = [&](const SVFVar* value) -> std::uintptr_t
    {
        const auto found = sourceVariableMap().find(value->getId());
        return found == sourceVariableMap().end() ? 0 : found->second;
    };

    // ---- expressions (remove_cast / remove_coeff / x = y + c) ----
    std::function<Expr(const SVFVar*, unsigned)> expr;
    const auto definitionExpr = [&](const SVFVar* value, unsigned depth) -> Expr
    {
        Expr e;
        const SVFStmt* statement = definition(value);
        if (const auto* copy = statement ? SVFUtil::dyn_cast<CopyStmt>(statement) : nullptr)
            return integerCast(copy) ? expr(copy->getRHSVar(), depth + 1) : e;
        if (const auto* binary = statement ? SVFUtil::dyn_cast<BinaryOPStmt>(statement) : nullptr)
        {
            const SVFVar* lhs = binary->getOpVar(0);
            const SVFVar* rhs = binary->getOpVar(1);
            const auto* lhsConstant = SVFUtil::dyn_cast<ConstIntValVar>(lhs);
            const auto* rhsConstant = SVFUtil::dyn_cast<ConstIntValVar>(rhs);
            const bool oneConstant = (lhsConstant != nullptr) != (rhsConstant != nullptr);
            const auto opcode = binary->getOpcode();
            if (opcode == BinaryOPStmt::Mul && oneConstant)
                return expr(lhsConstant ? rhs : lhs, depth + 1);
            if (opcode == BinaryOPStmt::Shl && rhsConstant && !lhsConstant)
                return expr(lhs, depth + 1);
            if ((opcode == BinaryOPStmt::Add || opcode == BinaryOPStmt::Sub) && oneConstant)
            {
                const Expr other = expr(lhsConstant ? rhs : lhs, depth + 1);
                if (other.kind != Expr::Var)
                    return e;
                const s64_t constant = (lhsConstant ? lhsConstant : rhsConstant)->getSExtValue();
                e.kind = Expr::VarPlusConst;
                e.var = other.var;
                e.plusOne = (opcode == BinaryOPStmt::Add && constant == 1) ||
                (opcode == BinaryOPStmt::Sub && constant == -1);
                return e;
            }
            if ((opcode == BinaryOPStmt::Add || opcode == BinaryOPStmt::Sub) &&
                    !lhsConstant && !rhsConstant)
            {
                const Expr left = expr(lhs, depth + 1), right = expr(rhs, depth + 1);
                if (left.kind == Expr::Var && right.kind == Expr::Var)
                {
                    e.kind = opcode == BinaryOPStmt::Add ? Expr::AddVars : Expr::SubVars;
                    e.var = left.var;
                    e.var2 = right.var;
                }
                return e;
            }
            return e;
        }
        if (statement && (SVFUtil::isa<CmpStmt>(statement) || SVFUtil::isa<UnaryOPStmt>(statement)))
            return e;
        e.kind = Expr::Var;
        e.var = value;
        return e;
    };
    expr = [&](const SVFVar* value, unsigned depth) -> Expr
    {
        Expr e;
        if (!integral(value))
        {
            e.kind = Expr::NonIntegral;
            return e;
        }
        if (SVFUtil::isa<ConstIntValVar>(value))
        {
            e.kind = Expr::Const;
            return e;
        }
        (void)depth;
        // D-014 item 7: every integer SSA value is a variable at the LLVM
        // level, named or not; an operand is never looked through.
        e.kind = Expr::Var;
        e.var = value;
        return e;
    };
    const auto stripIntegerCasts = [&](const SVFVar* value)
    {
        for (unsigned step = 0; step < 8; ++step)
        {
            const auto* copy = orNull<CopyStmt>(definition(value));
            if (!copy || !integerCast(copy))
                break;
            value = copy->getRHSVar();
        }
        return value;
    };

    // ---- commands (octImpactSem.set / forget / prune) ----
    const auto setVariable = [](const Locs& lv, const Locs& rv, bool weak)
    {
        Command command;
        command.kind = Command::SetVariable;
        command.lhs = lv.vars;
        command.rhs = rv.vars;
        command.strong = lv.strong && rv.strong;
        command.weakUpdate = weak;
        return command;
    };
    const auto forget = [](const Locs& lv, bool weak)
    {
        Command command;
        command.kind = Command::Forget;
        command.lhs = lv.vars;
        command.weakUpdate = weak;
        return command;
    };
    const auto overlaps = [&](const Locs& lv, const SVFVar* source)
    {
        // octImpactSem.set: x = x + 1 forgets when lv_set meets rv_set.
        if (lv.value && (lv.value == source ||
                         (sourceVariable(lv.value) && sourceVariable(lv.value) == sourceVariable(source))))
            return true; // SVF: SSA names of one source variable (mem2reg).
        if (const auto* load = orNull<LoadStmt>(definition(source)))
            for (const auto* object : pointees(load->getRHSVar()))
                if (lv.objects.count(object->getId()))
                    return true; // SVF: *p = *p + 1 through a load temporary.
        return false;
    };
    // Sparrow's x := x +/- c for a memory location: LLVM spells g = g + 1 as
    // t = load g; t2 = t + 1; store t2, g. Identify the location through the
    // stored value's definition so the store forgets as in octImpactSem.set.
    const auto storeExpr = [&](const Locs& lv, const SVFVar* value) -> Expr
    {
        const Expr direct = expr(value, 0);
        if (direct.kind != Expr::Var)
            return direct;
        const auto* binary = orNull<BinaryOPStmt>(definition(stripIntegerCasts(value)));
        if (!binary || (binary->getOpcode() != BinaryOPStmt::Add &&
                        binary->getOpcode() != BinaryOPStmt::Sub))
            return direct;
        const auto* lhsConstant = SVFUtil::dyn_cast<ConstIntValVar>(binary->getOpVar(0));
        const auto* rhsConstant = SVFUtil::dyn_cast<ConstIntValVar>(binary->getOpVar(1));
        if ((lhsConstant != nullptr) == (rhsConstant != nullptr))
            return direct;
        const SVFVar* loaded =
        stripIntegerCasts(lhsConstant ? binary->getOpVar(1) : binary->getOpVar(0));
        const auto* load = orNull<LoadStmt>(definition(loaded));
        if (!load)
            return direct;
        bool sameLocation = false;
        for (const auto* object : pointees(load->getRHSVar()))
            sameLocation |= lv.objects.count(object->getId()) != 0;
        if (!sameLocation)
            return direct;
        const s64_t constant = (lhsConstant ? lhsConstant : rhsConstant)->getSExtValue();
        Expr increment;
        increment.kind = Expr::VarPlusConst;
        increment.var = loaded;
        increment.plusOne = (binary->getOpcode() == BinaryOPStmt::Add && constant == 1) ||
        (binary->getOpcode() == BinaryOPStmt::Sub && constant == -1);
        return increment;
    };
    const auto compileSet = [&](const Locs& lv, const Expr& e, bool weak, Commands& out)
    {
        switch (e.kind)
        {
        case Expr::NonIntegral:
            return; // set: non-integral expressions leave the memory unchanged
        case Expr::Const:
        {
            Command command;
            command.kind = Command::SetConst;
            command.lhs = lv.vars;
            command.strong = lv.strong;
            command.weakUpdate = weak;
            out.push_back(command);
            return;
        }
        case Expr::Var:
            out.push_back(setVariable(lv, valueLocs(e.var), weak));
            return;
        case Expr::VarPlusConst:
            if (overlaps(lv, e.var))
            {
                if (e.plusOne)
                    out.push_back(forget(lv, weak));
                else if (lv.value)
                    // SVF: x := x - c over two SSA names; CIL would be a no-op
                    // on one variable, the closest SSA effect is a copy.
                    out.push_back(setVariable(lv, valueLocs(e.var), weak));
                return; // memory: set_variable x x is the identity
            }
            out.push_back(setVariable(lv, valueLocs(e.var), weak));
            return;
        default:
            out.push_back(forget(lv, weak));
            return;
        }
    };
    const auto compilePrune = [&](const CmpStmt* cmp, bool taken, bool weak, Commands& out)
    {
        auto op = comparison(cmp->getPredicate());
        if (!op)
            return;
        if (!taken)
            op = negate(*op);
        const Expr lhs = expr(cmp->getOpVar(0), 0), rhs = expr(cmp->getOpVar(1), 0);
        // CilHelper.make_cond_simple
        const SVFVar* x = nullptr;
        Expr right;
        enum { None, Plain, ConstWithVar } rightShape = None;
        if (lhs.kind == Expr::Var)
        {
            x = lhs.var;
            right = rhs;
        }
        else if (rhs.kind == Expr::Var)
        {
            x = rhs.var;
            right = lhs;
            op = reverse(*op);
        }
        else if ((lhs.kind == Expr::AddVars || lhs.kind == Expr::SubVars) && rhs.kind == Expr::Const)
        {
            // x + y op c  ->  x op c - y ;  x - y op c  ->  x op c + y
            x = lhs.var;
            right.kind = Expr::Var;
            right.var = lhs.var2;
            rightShape = ConstWithVar;
        }
        else
            return;
        if (rightShape == None)
            rightShape = Plain;
        // octImpactSem.prune: Lt/Le/Eq with Lval y or y +- c (or the
        // rewritten c -+ y) assume x y; Gt/Ge only with Lval y, reversed.
        const bool lessOrEqual = *op == Op::Lt || *op == Op::Le || *op == Op::Eq;
        const bool greater = *op == Op::Gt || *op == Op::Ge;
        const SVFVar* y = nullptr;
        if (right.kind == Expr::Var && rightShape == Plain && greater)
        {
            y = x;
            x = right.var;
        }
        else if (lessOrEqual && ((rightShape == Plain &&
                                  (right.kind == Expr::Var || right.kind == Expr::VarPlusConst)) ||
                                 rightShape == ConstWithVar))
            y = right.var;
        if (!y)
            return;
        const Locs xl = valueLocs(x), yl = valueLocs(y);
        Command command;
        command.kind = Command::Assume;
        command.lhs = xl.vars;
        command.rhs = yl.vars;
        command.strong = xl.strong && yl.strong;
        command.weakUpdate = weak;
        out.push_back(command);
    };

    // ---- compile every ICFG node and edge once ----
    std::unordered_map<const ICFGNode*, Commands> nodeCommands;
    std::map<std::pair<const ICFGNode*, const ICFGNode*>, Commands> edgeCommands;
    std::unordered_map<const SVFVar*, std::vector<const SVFVar*>> returnTargets;
    for (const SVFStmt* statement : ir_.getSVFStmtSet(SVFStmt::Ret))
    {
        const auto* binding = SVFUtil::cast<RetPE>(statement);
        returnTargets[binding->getRHSVar()].push_back(binding->getLHSVar());
    }
    const auto weakAt = [&](const ICFGNode* node)
    {
        return recursive(node->getFun());
    };
    std::size_t commandCount = 0;
    for (auto it = icfg->begin(); it != icfg->end(); ++it)
    {
        const ICFGNode* node = it->second;
        Commands& commands = nodeCommands[node];
        const bool weak = weakAt(node);
        for (const SVFStmt* statement : node->getSVFStmts())
        {
            if (const auto* address = SVFUtil::dyn_cast<AddrStmt>(statement))
            {
                // Calloc / Csalloc / realloc / calloc / strdup: Size(a) := e
                const AllocationSize* size = sizes_->find(address);
                if (!size)
                    continue;
                Locs lv;
                lv.strong = true; // OctLoc.Size: always strong
                if (const auto symbol = sizeOf(size->object))
                    lv.vars.push_back(*symbol);
                if (size->sizeSourcePointer)
                    commands.push_back(setVariable(lv, sizeLocsOfPointer(size->sizeSourcePointer), weak));
                else if (!size->known)
                    commands.push_back(forget(lv, weak));
                else if (!size->sizeValue)
                {
                    Expr constant;
                    constant.kind = Expr::Const;
                    compileSet(lv, constant, weak, commands);
                }
                else
                    compileSet(lv, expr(size->sizeValue, 0), weak, commands);
            }
            else if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement))
            {
                if (integral(load->getLHSVar()))
                    commands.push_back(setVariable(valueLocs(load->getLHSVar()),
                                                   contentLocs(load->getRHSVar()), weak));
            }
            else if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(statement))
            {
                const Locs target = contentLocs(store->getLHSVar());
                compileSet(target, storeExpr(target, store->getRHSVar()), weak, commands);
            }
            else if (const auto* select = SVFUtil::dyn_cast<SelectStmt>(statement))
            {
                // CIL lowers ?: to two assignments joined at the merge.
                if (!integral(select->getRes()))
                    continue;
                Command command;
                command.kind = Command::Alternatives;
                command.alternatives.resize(2);
                compileSet(valueLocs(select->getRes()), expr(select->getTrueValue(), 0), weak,
                           command.alternatives[0]);
                compileSet(valueLocs(select->getRes()), expr(select->getFalseValue(), 0), weak,
                           command.alternatives[1]);
                commands.push_back(command);
            }
            else if (SVFUtil::isa<CopyStmt>(statement) || SVFUtil::isa<BinaryOPStmt>(statement) ||
                     SVFUtil::isa<CmpStmt>(statement) || SVFUtil::isa<UnaryOPStmt>(statement))
            {
                const SVFVar* target = SVFUtil::isa<CopyStmt>(statement)
                                       ? SVFUtil::cast<CopyStmt>(statement)->getLHSVar()
                                       : SVFUtil::isa<UnaryOPStmt>(statement)
                                       ? SVFUtil::cast<UnaryOPStmt>(statement)->getRes()
                                       : SVFUtil::cast<MultiOpndStmt>(statement)->getRes();
                if (integral(target))
                    compileSet(valueLocs(target), definitionExpr(target, 0), weak, commands);
            }
        }
        if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
        {
            // octImpactSem.binding: formals := actuals, Weak with several callees.
            std::vector<const CallCFGEdge*> calls;
            for (const auto* edge : call->getOutEdges())
                if (const auto* callEdge = SVFUtil::dyn_cast<CallCFGEdge>(edge))
                    calls.push_back(callEdge);
            const bool several = calls.size() > 1;
            for (const auto* edge : calls)
                for (const auto* binding : edge->getCallPEs())
                    for (u32_t i = 0; i < binding->getOpVarNum(); ++i)
                        if (binding->getOpCallICFGNode(i) == call && integral(binding->getRes()))
                        {
                            Locs formal = valueLocs(binding->getRes());
                            if (several)
                                formal.strong = false;
                            compileSet(formal, expr(binding->getOpVar(i), 0), weak || several, commands);
                        }
        }
        if (const auto* ret = SVFUtil::dyn_cast<RetICFGNode>(node))
        {
            const CallICFGNode* call = ret->getCallICFGNode();
            bool internal = false;
            for (const auto* edge : call->getOutEdges())
                internal |= SVFUtil::isa<CallCFGEdge>(edge);
            const SVFVar* result = ret->getActualRet();
            if (!internal)
            {
                // handle_undefined_functions
                const FunObjVar* callee = call->getCalledFunction();
                const std::string name = callee ? callee->getName() : "";
                if (name == "strlen" && result && integral(result) && call->arg_size())
                {
                    const Locs lv = valueLocs(result);
                    commands.push_back(forget(lv, weak));
                    const Locs sizes = sizeLocsOfPointer(call->getArgument(0));
                    Command command;
                    command.kind = Command::Assume;
                    command.lhs = lv.vars;
                    command.rhs = sizes.vars;
                    command.strong = lv.strong && sizes.strong;
                    command.weakUpdate = weak;
                    commands.push_back(command);
                }
                else if (result && integral(result))
                    commands.push_back(forget(valueLocs(result), weak));
            }
        }
        if (const auto* entry = SVFUtil::dyn_cast<FunEntryICFGNode>(node))
        {
            // sparrow_arg at the start of main: forget argc, Size(argv) := argc.
            const FunObjVar* function = entry->getFun();
            const auto& formals = entry->getFormalParms();
            if (function && function->getName() == "main" && formals.size() >= 2 &&
                    integral(formals[0]) && formals[1]->isPointer())
            {
                const Locs argc = valueLocs(formals[0]);
                commands.push_back(forget(argc, weak));
                commands.push_back(setVariable(sizeLocsOfPointer(formals[1]), argc, weak));
            }
        }
        commandCount += commands.size();
    }
    for (const SVFStmt* statement : ir_.getSVFStmtSet(SVFStmt::Phi))
    {
        const auto* phi = SVFUtil::cast<PhiStmt>(statement);
        if (SVFUtil::isa<RetValPN>(phi->getRes()))
        {
            // Creturn (Some e): Dump(pid) := e at the return instruction.
            Locs targets;
            const auto& actuals = returnTargets[phi->getRes()];
            for (const SVFVar* actual : actuals)
                if (const auto symbol = numeric(actual))
                    targets.vars.push_back(*symbol);
            targets.strong = actuals.size() == 1 && !recursive(actuals.front()->getFunction());
            // SVF: the main analysis returns through the callee's return
            // variable (ret -> FunRet -> caller result), so the value passes
            // through FunRet; strength is Sparrow's (Dump(pid) cardinality).
            const Locs returned = valueLocs(phi->getRes());
            for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
            {
                const ICFGNode* at = phi->getOpICFGNode(i);
                compileSet(returned, expr(phi->getOpVar(i), 0), weakAt(at), nodeCommands[at]);
                nodeCommands[at].push_back(setVariable(targets, returned, weakAt(at)));
            }
            continue;
        }
        if (!integral(phi->getRes()))
            continue;
        // SVF: a phi is the CIL assignment on its incoming edge. Without a
        // direct edge, fall back to the join of the alternatives at the node.
        const ICFGNode* target = phi->getICFGNode();
        bool direct = true;
        for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
            direct &= icfg->hasIntraICFGEdge(const_cast<ICFGNode*>(phi->getOpICFGNode(i)),
                                             const_cast<ICFGNode*>(target), ICFGEdge::IntraCF) != nullptr;
        if (!direct)
        {
            ++result.phiFallbacks;
            if (diagnostics)
            {
                std::cerr << "IMPACT_DIAG_PHI_FALLBACK target=" << target->getId()
                          << " kind=" << target->getNodeKind();
                for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
                    std::cerr << " op=" << phi->getOpICFGNode(i)->getId() << ':'
                              << phi->getOpICFGNode(i)->getNodeKind();
                std::cerr << '\n';
            }
            Command command;
            command.kind = Command::Alternatives;
            command.alternatives.resize(phi->getOpVarNum());
            for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
                compileSet(valueLocs(phi->getRes()), expr(phi->getOpVar(i), 0), weakAt(target),
                           command.alternatives[i]);
            auto& commands = nodeCommands[target];
            commands.insert(commands.begin(), command);
            continue;
        }
        for (u32_t i = 0; i < phi->getOpVarNum(); ++i)
        {
            const ICFGNode* from = phi->getOpICFGNode(i);
            compileSet(valueLocs(phi->getRes()), expr(phi->getOpVar(i), 0), weakAt(target),
                       edgeCommands[ {from, target}]);
        }
    }
    for (auto it = icfg->begin(); it != icfg->end(); ++it)
        for (const auto* edge : it->second->getOutEdges())
            if (const auto* intra = SVFUtil::dyn_cast<IntraCFGEdge>(edge))
                if (intra->getCondition() && !intra->isSwitchEdge())
                    if (const auto* cmp = orNull<CmpStmt>(definition(intra->getCondition())))
                    {
                        // Cassume runs before the phi assignments of the edge.
                        Commands prune;
                        compilePrune(cmp, intra->getSuccessorCondValue() != 0,
                                     weakAt(edge->getSrcNode()), prune);
                        auto& slot = edgeCommands[ {edge->getSrcNode(), edge->getDstNode()}];
                        slot.insert(slot.begin(), prune.begin(), prune.end());
                    }
    result.commands = commandCount;

    if (std::getenv("SVF_IMPACT_DUMP_COMMANDS"))
    {
        // Per-rule tests read these lines: kind, strong/weak, lhs -> rhs names.
        const auto name = [&](Index index)
        {
            const Var symbol = universe[index];
            if (const auto* value = adapter_.value(symbol))
                return "v:" + value->getName();
            if (const auto* object = adapter_.contentObject(symbol))
                return "o:" + object->getName();
            if (const auto* object = adapter_.sizeObject(symbol))
                return "s:" + object->getName();
            return std::string("?");
        };
        const char* kinds[] = {"const", "set", "forget", "assume", "alternatives"};
        std::function<void(const Command&, const std::string&)> dump =
            [&](const Command& command, const std::string& where)
        {
            std::cerr << "IMPACT_COMMAND " << where << ' ' << kinds[command.kind]
                      << (command.strong ? " strong" : " weak")
                      << (command.weakUpdate ? " join" : " replace") << " lhs=";
            for (Index x : command.lhs)
                std::cerr << name(x) << ',';
            std::cerr << " rhs=";
            for (Index y : command.rhs)
                std::cerr << name(y) << ',';
            std::cerr << '\n';
            for (std::size_t i = 0; i < command.alternatives.size(); ++i)
                for (const auto& step : command.alternatives[i])
                    dump(step, where + "/alt" + std::to_string(i));
        };
        for (const auto& [node, commands] : nodeCommands)
            for (const auto& command : commands)
                dump(command, "node=" + std::to_string(node->getId()) + " fn=" +
                     (node->getFun() ? node->getFun()->getName() : std::string("-")));
        for (const auto& [edge, commands] : edgeCommands)
            for (const auto& command : commands)
                dump(command, "edge=" + std::to_string(edge.first->getId()) + "->" +
                     std::to_string(edge.second->getId()) + " fn=" +
                     (edge.second->getFun() ? edge.second->getFun()->getName() : std::string("-")));
    }

    // ---- successors: no call-to-return bypass when the call has callees ----
    const ICFGNode* global = icfg->getGlobalICFGNode();
    const auto successors = [&](const ICFGNode* node)
    {
        std::vector<const ICFGNode*> next;
        bool internal = false;
        if (SVFUtil::isa<CallICFGNode>(node))
            for (const auto* edge : node->getOutEdges())
                internal |= SVFUtil::isa<CallCFGEdge>(edge);
        for (const auto* edge : node->getOutEdges())
        {
            if (internal && SVFUtil::isa<IntraCFGEdge>(edge) &&
                    SVFUtil::isa<RetICFGNode>(edge->getDstNode()))
                continue;
            next.push_back(edge->getDstNode());
        }
        if (node == global)
            next.insert(next.end(), entries_.begin(), entries_.end());
        return next;
    };
    Order order;
    {
        const std::size_t limit = icfg->getTotalNodeNum() + 1;
        std::size_t maximum = 0;
        for (auto it = icfg->begin(); it != icfg->end(); ++it)
            maximum = std::max<std::size_t>(maximum, it->first);
        order.rank.assign(std::max(limit, maximum + 1), UINT_MAX);
        order.head.assign(order.rank.size(), false);
        std::vector<unsigned> postorder;
        std::vector<char> state(order.rank.size(), 0);
        std::vector<std::pair<const ICFGNode*, std::vector<const ICFGNode*>>> stack;
        stack.push_back({global, successors(global)});
        state[global->getId()] = 1;
        while (!stack.empty())
        {
            auto& [node, pending] = stack.back();
            if (pending.empty())
            {
                postorder.push_back(node->getId());
                state[node->getId()] = 2;
                stack.pop_back();
                continue;
            }
            const ICFGNode* next = pending.back();
            pending.pop_back();
            if (state[next->getId()] == 1)
                order.head[next->getId()] = true; // retreating edge
            else if (state[next->getId()] == 0)
            {
                state[next->getId()] = 1;
                stack.push_back({next, successors(next)});
            }
        }
        unsigned rank = 0;
        for (auto it = postorder.rbegin(); it != postorder.rend(); ++it)
            order.rank[*it] = rank++;
    }

    // ---- SparseAnalysis.widening over the dense ICFG ----
    struct Successor
    {
        unsigned node;
        const Commands* commands;
    };
    std::vector<std::vector<Successor>> next(order.rank.size());
    std::vector<const Commands*> transfer(order.rank.size(), nullptr);
    for (auto it = icfg->begin(); it != icfg->end(); ++it)
    {
        const ICFGNode* node = it->second;
        transfer[node->getId()] = &nodeCommands[node];
        for (const ICFGNode* target : successors(node))
        {
            const auto commands = edgeCommands.find({node, target});
            next[node->getId()].push_back(
            {target->getId(), commands == edgeCommands.end() ? nullptr : &commands->second});
        }
    }
    const auto fixpoint = [&](auto prototype, std::size_t& transfers)
    {
        using G = decltype(prototype);
        std::vector<G> inputs(order.rank.size()), outputs(order.rank.size());
        std::vector<char> hasOutput(order.rank.size(), 0);
        std::set<std::pair<unsigned, unsigned>> worklist;
        inputs[global->getId()] = G::top();
        worklist.insert({order.rank[global->getId()], global->getId()});
        while (!worklist.empty())
        {
            const unsigned id = worklist.begin()->second;
            worklist.erase(worklist.begin());
            G output = inputs[id];
            if (transfer[id])
                applyAll(*transfer[id], output);
            ++transfers;
            if (order.head[id] && hasOutput[id])
                output = outputs[id].join(output); // widen = join
            if (hasOutput[id] ? output.le(outputs[id]) : output.bottom)
                continue;
            outputs[id] = output;
            hasOutput[id] = 1;
            for (const Successor& successor : next[id])
            {
                G propagated = output;
                if (successor.commands)
                    applyAll(*successor.commands, propagated);
                G merged = inputs[successor.node].join(propagated);
                if (merged != inputs[successor.node])
                {
                    inputs[successor.node] = std::move(merged);
                    worklist.insert({order.rank[successor.node], successor.node});
                }
            }
        }
        return inputs;
    };
    const std::vector<SVF::ImpactPagedGraph> inputs = fixpoint(SVF::ImpactPagedGraph{}, result.transfers);
    result.preanalysisSeconds = std::chrono::duration<double>(Clock::now() - start).count();

    // ---- octImpactAnalysis.inspect_alarm ----
    const auto extractionStart = Clock::now();
    std::set<SVF::RelationEdge> selected;
    const auto collect = [&](const ICFGNode* node, const char* kind,
                             std::optional<Index> size, std::optional<Index> index,
                             const SVF::ImpactPagedGraph* at = nullptr)
    {
        const SVF::ImpactPagedGraph& graph = at ? *at : inputs[node->getId()];
        const bool supported = size && index;
        const bool promising = supported && !graph.bottom && graph.hasPath(*size, *index);
        if (diagnostics)
            std::cerr << "IMPACT_DIAG_QUERY node=" << node->getId() << " kind=" << kind
                      << " reachable=" << !graph.bottom << " edges=" << graph.edges().size()
                      << " defined=" << graph.defined().size() << " supported=" << supported
                      << " promising=" << promising << '\n';
        ImpactPackingQuery query{node->getId(), kind, supported, promising, {}};
        if (promising)
            for (const auto& edge : graph.edges())
            {
                const SVF::RelationEdge variables{universe[edge.first], universe[edge.second]};
                query.snapshot.push_back(variables);
                selected.insert(variables);
            }
        result.queries.push_back(std::move(query));
    };
    const auto indexOf = [&](const SVFVar* value) -> std::optional<Index>
    {
        // check: Some (Lval x) or Some (BinOp (_, Lval x, Const _)).
        const Expr e = expr(value, 0);
        if (diagnostics)
            std::cerr << "IMPACT_DIAG_INDEX value=" << value->getId() << " kind=" << e.kind << '\n';
        if (e.kind != Expr::Var && e.kind != Expr::VarPlusConst)
            return std::nullopt;
        return numeric(e.var);
    };
    const auto singleSize = [&](const SVFVar* pointer) -> std::optional<Index>
    {
        // ArrayBlk.cardinal arr = 1: a single allocation site.
        std::set<const BaseObjVar*> bases;
        for (const auto* object : pointees(pointer))
            bases.insert(ir_.getBaseObject(object->getId()));
        if (diagnostics)
        {
            std::cerr << "IMPACT_DIAG_BASE pointer=" << pointer->getId() << " pts="
                      << pointsTo_.getPts(pointer->getId()).count() << " bases=" << bases.size();
            for (NodeID id : pointsTo_.getPts(pointer->getId()))
                std::cerr << " raw=" << id << ":" << ir_.getGNode(id)->getNodeKind();
            std::cerr << " value=" << pointer->toString().substr(0, 160);
            for (const auto* base : bases)
                std::cerr << ' ' << base->getId() << (sizes_->find(base) ? ":sized" : ":unsized")
                          << (SVFUtil::isa<HeapObjVar>(base) ? "/heap" : SVFUtil::isa<StackObjVar>(base)
                              ? "/stack" : SVFUtil::isa<GlobalObjVar>(base) ? "/global" : "/other");
            std::cerr << '\n';
        }
        if (bases.size() != 1)
            return std::nullopt;
        return sizeOf(*bases.begin());
    };
    for (auto it = icfg->begin(); it != icfg->end(); ++it)
    {
        const ICFGNode* node = it->second;
        for (const SVFStmt* statement : node->getSVFStmts())
            if (const auto* gep = SVFUtil::dyn_cast<GepStmt>(statement))
                for (const auto& part : gep->getOffsetVarAndGepTypePairVec())
                    if (!SVFUtil::isa<ConstIntValVar>(part.first))
                        collect(node, "array-index", singleSize(gep->getRHSVar()), indexOf(part.first));
        const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node);
        const FunObjVar* callee = call ? call->getCalledFunction() : nullptr;
        if (!callee)
            continue;
        const std::string name = callee->getName();
        if (isMemcpyLike(name) && call->arg_size() >= 3)
        {
            collect(node, "memcpy-length", singleSize(call->getArgument(0)), indexOf(call->getArgument(2)));
            // Sparrow inspect_aexp passes the pointer expression e2 for the
            // source query, which cannot name an integral index location.
            collect(node, "memcpy-source", singleSize(call->getArgument(1)), std::nullopt);
        }
        // Sparrow passes v2exp=None for these two string kinds; they never
        // select an impact snapshot, but remain in the query inventory.
        if ((name == "strcpy" || name == "strcat") && call->arg_size() >= 2)
            collect(node, name.c_str(), singleSize(call->getArgument(0)), std::nullopt);
        if (!assertQueries)
            continue;
        // Off by default (D-005): assertions are not Sparrow alarm
        // expressions. The operands of a supported comparison are related
        // if the graph before the check has a path in either direction.
        const auto assertion = [&](const CmpStmt* cmp, const char* kind,
                                   const SVF::ImpactPagedGraph& graph)
        {
            const auto op = cmp ? comparison(cmp->getPredicate()) : std::nullopt;
            std::optional<Index> lhs, rhs;
            if (op && *op != Op::Ne)
            {
                lhs = indexOf(cmp->getOpVar(0));
                rhs = indexOf(cmp->getOpVar(1));
            }
            if (lhs && rhs && !graph.bottom && graph.hasPath(*rhs, *lhs))
                collect(node, kind, rhs, lhs, &graph);
            else
                collect(node, kind, lhs, rhs, &graph);
        };
        if (name == "svf_assert" && call->arg_size())
            assertion(orNull<CmpStmt>(definition(call->getArgument(0))), "assert-condition",
                      inputs[node->getId()]);
        else if (name == "__assert_fail" || name == "__assert_rtn" ||
                 name == "__assert_func" || name == "__assert2")
        {
            // The guards on the paths into the failure block, read before the
            // branch prunes them.
            std::vector<const ICFGNode*> pending{node};
            std::set<const ICFGNode*> seen;
            while (!pending.empty())
            {
                const ICFGNode* current = pending.back();
                pending.pop_back();
                if (!seen.insert(current).second)
                    continue;
                for (const auto* edge : current->getInEdges())
                    if (const auto* intra = SVFUtil::dyn_cast<IntraCFGEdge>(edge))
                    {
                        if (intra->getCondition() && !intra->isSwitchEdge())
                            assertion(orNull<CmpStmt>(definition(intra->getCondition())),
                                      "standard-assert-failure", inputs[edge->getSrcNode()->getId()]);
                        else
                            pending.push_back(edge->getSrcNode());
                    }
            }
        }
    }
    result.edges.assign(selected.begin(), selected.end());
    result.packs = SVF::connectedComponentPacking(vocabulary, result.edges);
    result.extractionSeconds = std::chrono::duration<double>(Clock::now() - extractionStart).count();
    return result;
}
}
