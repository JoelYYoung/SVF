// SPDX-License-Identifier: AGPL-3.0-or-later
#include "SVF-LLVM/PaperGuidedScope.h"
#include "SVF-LLVM/LLVMModule.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

namespace SVF
{
namespace
{
std::string field(const std::string& value)
{
    return std::to_string(value.size()) + ":" + value;
}

[[noreturn]] void reject(const std::string& reason)
{
    throw std::invalid_argument("AE_PAPER_SCOPE: " + reason);
}

std::string fileKey(const llvm::DIFile* file)
{
    if (!file) reject("missing debug file");
    return field(file->getDirectory().str()) + field(file->getFilename().str());
}

bool executable(const llvm::Instruction& instruction)
{
    return !llvm::isa<llvm::DbgInfoIntrinsic>(instruction);
}

/// Pointer maps are lookup/identity tests ONLY. Emitted order comes from module
/// function/BB/instruction order and fixed parent-before-child recursion.
class MetadataCatalog
{
    struct Entry
    {
        const llvm::MDNode* node;
        std::string local;
        std::string structural;
        std::string firstSite;
        std::vector<std::size_t> parents;
    };
    std::vector<Entry> entries;
    std::map<const llvm::MDNode*, std::size_t> indices;
    std::set<const llvm::MDNode*> active;
    std::map<std::string, std::vector<std::size_t>> sameStructure;

public:
    std::size_t visit(const llvm::MDNode* node, const std::string& site)
    {
        if (!node) reject("missing metadata parent");
        auto found = indices.find(node);
        if (found != indices.end()) return found->second;
        if (!active.insert(node).second) reject("cyclic scope/inlinedAt graph");
        Entry entry{node, "", "", site, {}};
        if (const auto* location = llvm::dyn_cast<llvm::DILocation>(node))
        {
            entry.local = "location" + field(std::to_string(location->getLine())) +
                field(std::to_string(location->getColumn())) +
                field(std::to_string(location->getDiscriminator()));
            entry.parents.push_back(visit(location->getScope(), site + "/scope"));
            if (location->getInlinedAt())
                entry.parents.push_back(visit(location->getInlinedAt(), site + "/inlinedAt"));
        }
        else if (const auto* wrapper = llvm::dyn_cast<llvm::DILexicalBlockFile>(node))
        {
            entry.local = "block-file" + fileKey(wrapper->getFile()) +
                field(std::to_string(wrapper->getDiscriminator()));
            entry.parents.push_back(visit(wrapper->getScope(), site + "/parent"));
        }
        else if (const auto* block = llvm::dyn_cast<llvm::DILexicalBlock>(node))
        {
            entry.local = "block" + fileKey(block->getFile()) +
                field(std::to_string(block->getLine())) + field(std::to_string(block->getColumn()));
            entry.parents.push_back(visit(block->getScope(), site + "/parent"));
        }
        else if (const auto* subprogram = llvm::dyn_cast<llvm::DISubprogram>(node))
        {
            entry.local = "subprogram" + fileKey(subprogram->getFile()) +
                field(subprogram->getLinkageName().str()) + field(subprogram->getName().str()) +
                field(std::to_string(subprogram->getLine())) +
                field(std::to_string(subprogram->getScopeLine()));
            // Local lexical ancestry ends at the defining subprogram. Equal
            // displayed fields do not collapse distinct metadata nodes.
        }
        else reject("unsupported scope metadata kind");
        entry.structural = field(entry.local);
        for (auto parent : entry.parents) entry.structural += field(entries.at(parent).structural);
        active.erase(node);
        const auto index = entries.size();
        entries.push_back(std::move(entry));
        indices.emplace(node, index);
        sameStructure[entries.back().structural].push_back(index);
        return index;
    }

    void finish(PaperGuidedModuleScopes& result) const
    {
        std::vector<std::string> keys;
        std::map<std::string, const llvm::MDNode*> unique;
        for (std::size_t index = 0; index < entries.size(); ++index)
        {
            const auto& entry = entries[index];
            std::string key = field(result.moduleOrigin) + field(entry.local);
            for (auto parent : entry.parents) key += field(keys.at(parent));
            const auto& ties = sameStructure.at(entry.structural);
            if (ties.size() > 1)
            {
                // Rank only structural ties, in deterministic original-module
                // first-appearance order. It is not a process allocation ID.
                auto position = std::find(ties.begin(), ties.end(), index);
                key += field("first-appearance") + field(std::to_string(position - ties.begin()));
                if (index == ties.front())
                {
                    PaperGuidedMetadataTie tie;
                    tie.structuralKey = field(result.moduleOrigin) + field(entry.structural);
                    for (auto other : ties) tie.firstAppearanceSites.push_back(entries[other].firstSite);
                    result.disambiguations.push_back(std::move(tie));
                }
            }
            if (!unique.emplace(key, entry.node).second) reject("metadata identity collision");
            keys.push_back(key);
            result.metadataKeys.emplace(entry.node, std::move(key));
        }
    }
};

const std::string& functionOrigin(const llvm::Function& function,
                                 const PaperGuidedFunctionOrigins& origins)
{
    auto found = origins.find(&function);
    if (found == origins.end() || found->second.empty()) reject("missing function origin");
    return found->second;
}

/// Rebuild only from CURRENT IR references. Old pointer-map entries are opaque
/// lookup tokens: never traverse/dereference them, even to check liveness.
/// Recomputed parent keys and tie ranks detect structure or rank changes. Exact
/// first-use evidence for each tie is required; if preprocessing shifts it we
/// conservatively reject, rather than inherit an unverified distinct-node rank.
void validateCurrentMetadata(const llvm::Module& module,
                             const PaperGuidedModuleScopes& original,
                             const PaperGuidedFunctionOrigins& origins,
                             const std::set<std::string>& modelOrigins)
{
    MetadataCatalog live;
    for (const auto& function : module)
    {
        const auto& origin = functionOrigin(function, origins);
        if (modelOrigins.count(origin)) continue;
        if (origin != original.moduleOrigin) reject("program function origin/catalog mismatch");
        if (!function.getSubprogram()) continue;
        live.visit(function.getSubprogram(), "fn=" + function.getName().str() + "/subprogram");
        for (const auto& block : function)
            for (const auto& instruction : block)
                if (executable(instruction) && instruction.getDebugLoc())
                    live.visit(instruction.getDebugLoc().get(),
                               LLVMModuleSet::instructionSemanticSite(&instruction));
    }
    PaperGuidedModuleScopes current;
    current.moduleOrigin = original.moduleOrigin;
    live.finish(current);
    // Entries here were reached from live IR, not old snapshot pointers.
    for (const auto& entry : current.metadataKeys)
    {
        auto old = original.metadataKeys.find(entry.first);
        if (old == original.metadataKeys.end()) reject("metadata absent from original module inventory");
        if (old->second != entry.second) reject("current metadata structure or tie rank changed");
    }
    if (current.disambiguations.size() != original.disambiguations.size())
        reject("metadata tie inventory changed");
    for (std::size_t i = 0; i < current.disambiguations.size(); ++i)
    {
        const auto& now = current.disambiguations[i];
        const auto& old = original.disambiguations[i];
        if (now.structuralKey != old.structuralKey || now.firstAppearanceSites != old.firstAppearanceSites)
            reject("metadata tie first-appearance evidence changed");
    }
}

const llvm::DILocalScope* lexicalScope(const llvm::DILocalScope* scope)
{
    std::set<const llvm::DILocalScope*> seen;
    while (const auto* wrapper = llvm::dyn_cast_or_null<llvm::DILexicalBlockFile>(scope))
    {
        if (!seen.insert(scope).second) reject("cyclic lexical wrapper");
        scope = wrapper->getScope();
    }
    if (!scope) reject("missing lexical scope");
    return scope;
}

const std::string& metadataKey(const PaperGuidedModuleScopes& original, const llvm::MDNode* node)
{
    auto found = original.metadataKeys.find(node);
    if (found == original.metadataKeys.end()) reject("metadata absent from original module inventory");
    return found->second;
}

PaperGuidedScope recover(const llvm::Instruction& instruction, const PaperGuidedModuleScopes& original,
                         bool modelExcluded)
{
    PaperGuidedScope result;
    if (modelExcluded) result.unknownReason = "model-excluded";
    else if (!instruction.getFunction()->getSubprogram()) result.unknownReason = "function-no-subprogram";
    else if (!instruction.getDebugLoc()) result.unknownReason = "instruction-no-location";
    if (!result.unknownReason.empty()) return result;
    const auto* location = instruction.getDebugLoc().get();
    // Validate the original location, but do not make each statement location
    // into a different C block. Inline call sites are included separately.
    metadataKey(original, location);
    const auto* source = lexicalScope(location->getScope());
    result.sourceFunction = metadataKey(original, source->getSubprogram());
    result.lexicalKey = field(metadataKey(original, source));
    std::set<const llvm::DILocation*> visited;
    for (auto* at = location->getInlinedAt(); at; at = at->getInlinedAt())
    {
        if (!visited.insert(at).second) reject("cyclic inlinedAt chain");
        const auto& key = metadataKey(original, at);
        result.inlineCallChain.push_back(key);
        result.lexicalKey += field(key);
    }
    return result;
}
}

PaperGuidedModuleScopes preparePaperGuidedModuleScopes(
    const llvm::Module& module, const std::string& moduleOrigin, bool isModel)
{
    const std::string prefix = "module-sha256=";
    if (moduleOrigin.size() != prefix.size() + 64 ||
        moduleOrigin.compare(0, prefix.size(), prefix) != 0 ||
        !std::all_of(moduleOrigin.begin() + prefix.size(), moduleOrigin.end(),
                     [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        reject("missing or invalid module provenance");
    PaperGuidedModuleScopes result;
    result.originalModule = &module;
    result.moduleOrigin = moduleOrigin;
    result.modelExcluded = isModel;
    if (isModel) return result;
    const auto* units = module.getNamedMetadata("llvm.dbg.cu");
    result.compileUnits = units ? units->getNumOperands() : 0;
    if (!result.compileUnits) reject("G9 original program module has no debug compile unit");
    MetadataCatalog catalog;
    // Deterministic order: module functions, then blocks and instructions;
    // recursion always visits parent scope before the inline call-site parent.
    for (const auto& function : module)
    {
        if (const auto* subprogram = function.getSubprogram())
            catalog.visit(subprogram, "fn=" + function.getName().str() + "/subprogram");
        for (const auto& block : function)
            for (const auto& instruction : block)
                if (executable(instruction) && function.getSubprogram() && instruction.getDebugLoc())
                {
                    catalog.visit(instruction.getDebugLoc().get(),
                                  LLVMModuleSet::instructionSemanticSite(&instruction));
                    ++result.locatedInstructions;
                }
    }
    if (!result.locatedInstructions) reject("G9 original program module has no resolvable executable location");
    catalog.finish(result);
    return result;
}

PaperGuidedScopeInventory recoverPaperGuidedScopes(
    const llvm::Function& function, const PaperGuidedModuleScopes& original,
    const PaperGuidedFunctionOrigins& functionOrigins,
    const std::set<std::string>& modelModuleOrigins)
{
    const auto& origin = functionOrigin(function, functionOrigins);
    const bool modelExcluded = modelModuleOrigins.count(origin) != 0;
    if (!modelExcluded)
    {
        if (original.modelExcluded || origin != original.moduleOrigin ||
            function.getParent() != original.originalModule)
            reject("program function origin/catalog mismatch");
        validateCurrentMetadata(*function.getParent(), original, functionOrigins, modelModuleOrigins);
    }
    PaperGuidedScopeInventory inventory;
    inventory.modelExcluded = modelExcluded;
    inventory.functionScopeUnknown = !function.isDeclaration() && !function.getSubprogram();
    if (function.isDeclaration()) return inventory;
    for (const auto& block : function)
        for (const auto& instruction : block)
            if (executable(instruction))
            {
                auto scope = recover(instruction, original, modelExcluded);
                if (!scope.unknownReason.empty()) ++inventory.unknownByOpcode[instruction.getOpcodeName()];
                inventory.instructions.emplace(&instruction, std::move(scope));
            }
    if (modelExcluded) return inventory;

    // These analyses build side data, not transformations or new SVF objects.
    llvm::DominatorTree dominators(const_cast<llvm::Function&>(function));
    llvm::LoopInfo loops(dominators);
    std::vector<const llvm::Loop*> pending(loops.begin(), loops.end());
    for (std::size_t index = 0; index < pending.size(); ++index)
    {
        const auto* loop = pending[index];
        pending.insert(pending.end(), loop->getSubLoops().begin(), loop->getSubLoops().end());
        PaperGuidedLoop result;
        result.header = loop->getHeader()->getTerminator();
        result.headerSite = field(original.moduleOrigin) +
            field(LLVMModuleSet::instructionSemanticSite(result.header));
        std::set<std::string> scopes;
        for (const auto& instruction : *loop->getHeader())
        {
            if (!executable(instruction)) continue;
            const auto& scope = inventory.instructions.at(&instruction);
            if (scope.lexicalKey.empty()) ++result.unknownHeaderInstructions;
            else
            {
                scopes.insert(scope.lexicalKey);
                result.scopeContributors.push_back(LLVMModuleSet::instructionSemanticSite(&instruction));
            }
        }
        result.knownHeaderScopes.assign(scopes.begin(), scopes.end());
        if (scopes.size() != 1)
        {
            result.exclusionReason = scopes.empty() ? "no-resolvable-scope" : "multiple-scopes";
            inventory.loops.push_back(std::move(result));
            continue;
        }
        // Loop label only. Unknown instructions in the header stay unknown.
        result.headerScope = *scopes.begin();
        llvm::SmallVector<llvm::BasicBlock*, 4> exiting;
        loop->getExitingBlocks(exiting);
        for (const auto* block : exiting)
        {
            const auto* end = block->getTerminator();
            result.exits.push_back(end);
            if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(end))
            {
                if (branch->isConditional()) result.exitConditions.push_back(branch->getCondition());
            }
            else if (const auto* sw = llvm::dyn_cast<llvm::SwitchInst>(end))
                result.exitConditions.push_back(sw->getCondition());
            else result.unsupportedExits.emplace_back(
                LLVMModuleSet::instructionSemanticSite(end), end->getOpcodeName());
        }
        // Candidate heuristic, not a proof of monotonic induction: header phi
        // updated by phi +/- invariant and with an outside incoming edge.
        for (const auto& phi : loop->getHeader()->phis())
        {
            bool outside = false;
            std::vector<const llvm::Instruction*> updates;
            for (unsigned i = 0; i < phi.getNumIncomingValues(); ++i)
            {
                if (!loop->contains(phi.getIncomingBlock(i))) { outside = true; continue; }
                const auto* update = llvm::dyn_cast<llvm::BinaryOperator>(phi.getIncomingValue(i));
                if (!update) continue;
                const bool add = update->getOpcode() == llvm::Instruction::Add;
                const bool sub = update->getOpcode() == llvm::Instruction::Sub;
                if (((add || sub) && update->getOperand(0) == &phi && loop->isLoopInvariant(update->getOperand(1))) ||
                    (add && update->getOperand(1) == &phi && loop->isLoopInvariant(update->getOperand(0))))
                    updates.push_back(update);
            }
            if (outside && !updates.empty())
            {
                result.inductionPhis.push_back(&phi);
                result.inductionUpdates.insert(result.inductionUpdates.end(), updates.begin(), updates.end());
            }
        }
        inventory.loops.push_back(std::move(result));
    }
    return inventory;
}
}
