// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_PAPER_GUIDED_SCOPE_H
#define SVF_PAPER_GUIDED_SCOPE_H

#include <map>
#include <cstddef>
#include <string>
#include <set>
#include <vector>
#include <utility>

namespace llvm
{
class Function;
class Instruction;
class Value;
class Module;
class MDNode;
}

namespace SVF
{
/// LLVM-side, opt-in metadata only. No caller is enabled by this first slice.
/// Pointer keys are temporary lookup handles, never serialized pack identities.
struct PaperGuidedScope
{
    std::string lexicalKey;
    std::string sourceFunction;
    std::vector<std::string> inlineCallChain;
    std::string unknownReason;
};

struct PaperGuidedMetadataTie
{
    std::string structuralKey;
    std::vector<std::string> firstAppearanceSites;
};

/// Capture BEFORE preprocessing and model cloning, one per original module.
/// Pointer keys are lookup handles only, never serialized or iterated for IDs.
struct PaperGuidedModuleScopes
{
    const llvm::Module* originalModule = nullptr;
    std::string moduleOrigin;
    bool modelExcluded = false;
    std::size_t compileUnits = 0;
    std::size_t locatedInstructions = 0;
    std::map<const llvm::MDNode*, std::string> metadataKeys;
    std::vector<PaperGuidedMetadataTie> disambiguations;
};

struct PaperGuidedLoop
{
    const llvm::Instruction* header = nullptr;
    std::string headerScope;
    std::string headerSite;
    std::vector<std::string> knownHeaderScopes;
    std::vector<std::string> scopeContributors;
    std::size_t unknownHeaderInstructions = 0;
    std::string exclusionReason;
    std::vector<const llvm::Instruction*> exits;
    std::vector<const llvm::Value*> exitConditions;
    // site/opcode of exits with no supported candidate extraction rule.
    std::vector<std::pair<std::string, std::string>> unsupportedExits;
    std::string inductionRule = "header-phi-add-sub-invariant-with-outside-edge";
    std::vector<const llvm::Instruction*> inductionPhis;
    std::vector<const llvm::Instruction*> inductionUpdates;
};

struct PaperGuidedScopeInventory
{
    std::map<const llvm::Instruction*, PaperGuidedScope> instructions;
    std::vector<PaperGuidedLoop> loops;
    std::map<std::string, std::size_t> unknownByOpcode;
    bool functionScopeUnknown = false;
    bool modelExcluded = false;
};

/// Supplied from the frontend's pre-clone functionOrigins plus clone routing.
/// Neither function name nor current parent module determines model origin.
using PaperGuidedFunctionOrigins = std::map<const llvm::Function*, std::string>;

/// Dormant helpers: paper-policy integration is a separate reviewed step.
/// No SVF objects are created and no IR/debug metadata is modified.
/// moduleOrigin must be "module-sha256=" followed by 64 lowercase hex digits.
PaperGuidedModuleScopes preparePaperGuidedModuleScopes(
    const llvm::Module& module, const std::string& moduleOrigin, bool isModel);

/// Recover after preprocessing using the original metadata catalog. Missing
/// locations are unknown; unknown metadata identity fails closed. Model bodies
/// are inventoried as excluded. An unresolved loop excludes only its candidate.
PaperGuidedScopeInventory recoverPaperGuidedScopes(
    const llvm::Function& function, const PaperGuidedModuleScopes& original,
    const PaperGuidedFunctionOrigins& functionOrigins,
    const std::set<std::string>& modelModuleOrigins);
}
#endif
