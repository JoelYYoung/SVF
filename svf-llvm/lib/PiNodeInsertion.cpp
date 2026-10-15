// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SVF-LLVM/PiNodeInsertion.h"

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include <vector>

using namespace llvm;

namespace SVF
{
namespace
{
bool piCandidate(const Value* value)
{
    return value && (isa<Instruction>(value) || isa<Argument>(value)) &&
           value->getType()->isIntegerTy() &&
           value->getType()->getIntegerBitWidth() > 1;
}

const ICmpInst* branchComparison(const BranchInst* branch)
{
    if (!branch || !branch->isConditional())
        return nullptr;
    const auto* comparison = dyn_cast<ICmpInst>(branch->getCondition());
    return comparison && comparison->getOperand(0)->getType()->isIntegerTy()
           ? comparison : nullptr;
}

bool switchCandidate(const SwitchInst* select)
{
    return select && piCandidate(select->getCondition());
}

/// Successors of a switch that are reached by exactly one non-default case.
std::vector<BasicBlock*> singleCaseSuccessors(SwitchInst* select)
{
    std::vector<BasicBlock*> result;
    for (auto& entry : select->cases())
    {
        BasicBlock* successor = entry.getCaseSuccessor();
        unsigned edges = 0;
        for (unsigned i = 0; i < select->getNumSuccessors(); ++i)
            edges += select->getSuccessor(i) == successor;
        if (edges == 1 && successor != select->getDefaultDest())
            result.push_back(successor);
    }
    return result;
}

/// Give every chosen successor a single predecessor. Returns the edges split.
unsigned splitEdges(Function& function)
{
    std::vector<std::pair<BasicBlock*, BasicBlock*>> edges;
    for (BasicBlock& block : function)
    {
        Instruction* terminator = block.getTerminator();
        if (auto* branch = dyn_cast<BranchInst>(terminator); branchComparison(branch))
        {
            BasicBlock* first = branch->getSuccessor(0);
            BasicBlock* second = branch->getSuccessor(1);
            if (first == second)
                continue;
            for (BasicBlock* successor : {first, second})
                if (!successor->getSinglePredecessor())
                    edges.emplace_back(&block, successor);
        }
        else if (auto* select = dyn_cast<SwitchInst>(terminator); switchCandidate(select))
        {
            for (BasicBlock* successor : singleCaseSuccessors(select))
                if (!successor->getSinglePredecessor())
                    edges.emplace_back(&block, successor);
        }
    }
    for (const auto& [from, to] : edges)
        SplitEdge(from, to);
    return edges.size();
}

/// Insert x.pi at the start of `successor` and rename the uses of x it
/// dominates.
void insertPi(Value* operand, Value* condition, BasicBlock* successor,
              DominatorTree& tree, PiNodeStatistics& statistics)
{
    auto* pi = SelectInst::Create(condition, operand, operand,
                                  operand->getName() + ".pi",
                                  successor->getFirstInsertionPt());
    ++statistics.piNodes;
    std::vector<Use*> uses;
    for (Use& use : operand->uses())
        uses.push_back(&use);
    for (Use* use : uses)
    {
        auto* user = dyn_cast<Instruction>(use->getUser());
        if (!user || user == pi)
            continue;
        const BasicBlock* useBlock = user->getParent();
        if (const auto* phi = dyn_cast<PHINode>(user))
            useBlock = phi->getIncomingBlock(*use);
        // Phis of `successor` read their operand on the incoming edge, which
        // the pi does not dominate; any other use in `successor` follows it.
        if (useBlock == successor && isa<PHINode>(user))
            continue;
        if (!tree.dominates(successor, useBlock))
            continue;
        use->set(pi);
        ++statistics.renamedUses;
    }
}
} // namespace

const Value* piOperand(const SelectInst* select)
{
    return select && select->getTrueValue() == select->getFalseValue()
           ? select->getTrueValue() : nullptr;
}

PiNodeStatistics insertPiNodes(Module& module)
{
    PiNodeStatistics statistics;
    for (Function& function : module)
    {
        if (function.isDeclaration())
            continue;
        statistics.splitEdges += splitEdges(function);
        DominatorTree tree(function);
        // Outer branches first, so an inner comparison already reads the
        // outer pi and receives a pi of that pi.
        std::vector<BasicBlock*> order;
        for (auto* node : depth_first(tree.getRootNode()))
            order.push_back(node->getBlock());
        for (BasicBlock* block : order)
        {
            Instruction* terminator = block->getTerminator();
            if (auto* branch = dyn_cast<BranchInst>(terminator))
            {
                const ICmpInst* comparison = branchComparison(branch);
                if (!comparison || branch->getSuccessor(0) == branch->getSuccessor(1))
                    continue;
                ++statistics.branches;
                // Allocation addresses vary across processes. Preserve the
                // comparison's operand order so pi names, instruction sites
                // and downstream variable identities are reproducible.
                std::vector<Value*> operands;
                for (Value* operand : comparison->operands())
                    if (piCandidate(operand) &&
                            (operands.empty() || operands.front() != operand))
                        operands.push_back(operand);
                for (BasicBlock* successor : {branch->getSuccessor(0), branch->getSuccessor(1)})
                {
                    if (successor->getSinglePredecessor() != block)
                        continue;
                    for (Value* operand : operands)
                        insertPi(operand, branch->getCondition(), successor, tree, statistics);
                }
            }
            else if (auto* select = dyn_cast<SwitchInst>(terminator); switchCandidate(select))
            {
                ++statistics.switches;
                for (BasicBlock* successor : singleCaseSuccessors(select))
                    if (successor->getSinglePredecessor() == block)
                        insertPi(select->getCondition(),
                                 ConstantInt::getTrue(module.getContext()),
                                 successor, tree, statistics);
            }
        }
    }
    return statistics;
}
} // namespace SVF
