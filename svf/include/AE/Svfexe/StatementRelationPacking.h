// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_AE_STATEMENT_RELATION_PACKING_H
#define SVF_AE_STATEMENT_RELATION_PACKING_H

#include "AE/Core/RelationPacking.h"

namespace SVF
{
class SVFIR;
class SVFIRAdapter;
class PointerAnalysis;
class AllocationSizeModel;

struct StatementPackingEdge
{
    AbstractDomain::RelationEdge variables;
    unsigned statement;
    const char* kind;
};

struct StatementPackingResult
{
    AbstractDomain::RelationPacking packs;
    std::vector<StatementPackingEdge> edges;
};

/// Bind the program context once, then pack any permitted numerical vocabulary.
/// Points-to identifies possible content dependencies, never numeric facts.
class StatementRelationPacking
{
public:
    /// With `sizes`, D-005 adds Size(o)-size and Size(o)-index edges.
    StatementRelationPacking(SVFIR& ir, const SVFIRAdapter& adapter,
                             PointerAnalysis& pointsTo,
                             const AllocationSizeModel* sizes = nullptr)
        : ir_(ir), adapter_(adapter), pointsTo_(pointsTo), sizes_(sizes) {}
    StatementPackingResult pack(
        const std::vector<AbstractDomain::Variable>& vocabulary) const;
private:
    SVFIR& ir_;
    const SVFIRAdapter& adapter_;
    PointerAnalysis& pointsTo_;
    const AllocationSizeModel* sizes_;
};
}
#endif
