// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_AE_ALLOCATION_SIZE_H
#define SVF_AE_ALLOCATION_SIZE_H

#include "Util/GeneralType.h"
#include <map>
#include <vector>

namespace SVF
{
class AddrStmt;
class BaseObjVar;
class CallICFGNode;
class SVFIR;
class SVFVar;
class ValVar;

/// Sparrow's Size(a) coordinate of one allocation site, in element units.
/// The element count is `operand` (a syntactically stripped SSA value) or
/// `constant`; one element is `elementBytes` bytes (0: unit unknown).
/// Stripping follows Sparrow's remove_cast/remove_coeff: integer casts and
/// multiplications by a constant are removed. LLVM has folded sizeof, so a
/// literal multiplier is stripped as well. Users that need exact values
/// (the main analysis) must recheck that the stripped steps cannot wrap.
struct AllocationSize
{
    const BaseObjVar* object = nullptr;
    const AddrStmt* site = nullptr;
    /// Original allocation-size value whose bytes/elementBytes is Size(o);
    /// null for constant-size objects.
    const SVFVar* sizeValue = nullptr;
    const ValVar* operand = nullptr;
    s64_t constant = 0;
    u32_t elementBytes = 0;
    /// Bytes per unit of `sizeValue` (alloca element size, 1 for byte
    /// allocators; 0 when sizeValue alone does not give the byte count).
    u32_t unitBytes = 0;
    /// Multiplier removed between `sizeValue` and `operand`.
    s64_t strippedCoefficient = 1;
    /// False: the site allocates an object with an unknown size (for
    /// example strdup or an unmodelled allocator). Size(o) is forgotten.
    bool known = false;
    /// For library models that copy another object's size (strdup).
    const SVFVar* sizeSourcePointer = nullptr;
};

class AllocationSizeModel
{
public:
    explicit AllocationSizeModel(SVFIR& ir);
    const AllocationSize* find(const BaseObjVar* object) const;
    const AllocationSize* find(const AddrStmt* site) const;
    const std::vector<AllocationSize>& all() const { return sizes_; }

    /// Look through integer casts (remove_cast).
    static const SVFVar* stripCasts(const SVFVar* value);
    /// Look through integer casts and constant multipliers (remove_coeff).
    static const SVFVar* stripCoefficient(const SVFVar* value, s64_t& coefficient,
                                          std::vector<const SVFVar*>* chain = nullptr);
    /// The call that allocates a heap object, if the site is a call.
    static const CallICFGNode* allocationCall(const AddrStmt* site);

private:
    std::vector<AllocationSize> sizes_;
    std::map<const BaseObjVar*, std::size_t> byObject_;
    std::map<const AddrStmt*, std::size_t> bySite_;
};
} // namespace SVF
#endif
