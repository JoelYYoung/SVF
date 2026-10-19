//===- PiNodeInsertion.h -- Pi Node Insertion --------===//
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
#ifndef SVF_LLVM_PI_NODE_INSERTION_H
#define SVF_LLVM_PI_NODE_INSERTION_H

namespace llvm
{
class Module;
class Value;
class SelectInst;
}

namespace SVF
{
/// e-SSA preparation (research D-006). For every conditional branch on an
/// integer comparison, and every switch case on an integer value, each
/// non-constant integer operand gets a pi definition at the start of the
/// successor (critical edges are split first):
///     x.pi = select i1 %cond, x, x        ; branch successor
///     x.pi = select i1 true, x, x         ; switch case successor
/// and the uses of x dominated by that successor are renamed to x.pi. The
/// abstract interpreter recognises the pattern through the block's single
/// incoming edge and executes it as x.pi := x followed by the edge's
/// comparison (or case range) on x.pi.
struct PiNodeStatistics
{
    unsigned branches = 0;
    unsigned switches = 0;
    unsigned piNodes = 0;
    unsigned renamedUses = 0;
    unsigned splitEdges = 0;
};

PiNodeStatistics insertPiNodes(llvm::Module& module);

/// A pi node is a select whose two values are the same.
const llvm::Value* piOperand(const llvm::SelectInst* select);
} // namespace SVF

#endif
