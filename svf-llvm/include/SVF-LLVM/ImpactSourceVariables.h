//===- ImpactSourceVariables.h -- Impact Source Variables --------===//
//
//                     SVF: Static Value-Flow Analysis
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_LLVM_IMPACT_SOURCE_VARIABLES_H
#define SVF_LLVM_IMPACT_SOURCE_VARIABLES_H
#include "AE/Svfexe/ImpactRelationPacking.h"
#include "SVF-LLVM/LLVMModule.h"
#include "SVF-LLVM/PiNodeInsertion.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugProgramInstruction.h"
#include "llvm/IR/IntrinsicInst.h"
#include <map>

namespace SVF
{
/// Register each SSA value's debug-info source variable with the impact
/// port. Sparrow's x := x + 1 rule compares CIL variables; after mem2reg
/// they survive only as debug values (research D-005, project T-011).
inline void registerImpactSourceVariables()
{
    std::map<NodeID, std::uintptr_t> sources;
    const auto& symbols = LLVMModuleSet::getLLVMModuleSet()->valSyms();
    for (const auto& [value, id] : symbols)
    {
        if (!value || !(llvm::isa<llvm::Instruction>(value) || llvm::isa<llvm::Argument>(value)))
            continue;
        // A pi node (D-006) is the same source variable as its operand.
        const llvm::Value* origin = value;
        for (unsigned step = 0; step < 64; ++step)
        {
            const auto* select = llvm::dyn_cast<llvm::SelectInst>(origin);
            const llvm::Value* operand = select ? piOperand(select) : nullptr;
            if (!operand)
                break;
            origin = operand;
        }
        llvm::SmallVector<llvm::DbgValueInst*, 2> intrinsics;
        llvm::SmallVector<llvm::DbgVariableRecord*, 2> records;
        llvm::findDbgValues(intrinsics, const_cast<llvm::Value*>(origin), &records);
        const llvm::DILocalVariable* variable = nullptr;
        for (const auto* record : records)
            if (!variable && record->getVariable())
                variable = record->getVariable();
        for (const auto* intrinsic : intrinsics)
            if (!variable && intrinsic->getVariable())
                variable = intrinsic->getVariable();
        if (variable)
            sources[id] = reinterpret_cast<std::uintptr_t>(variable);
    }
    ImpactRelationPacking::setSourceVariables(std::move(sources));
}
} // namespace SVF
#endif
