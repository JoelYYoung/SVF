// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_AE_IMPACT_RELATION_GRAPH_H
#define SVF_AE_IMPACT_RELATION_GRAPH_H
#include "AE/Core/RelationPacking.h"
#include <set>

namespace SVF::AbstractDomain
{
/// Adaptation of Sparrow octImpactDom.AbsOct (see source attribution in ImpactRelationGraph.cpp).
/// An edge y -> x predicts a finite upper bound on x-y. These predictions
/// select tracking only; they are not a sound numerical abstract state.
struct ImpactRelationGraph
{
    bool bottom = true;
    std::set<RelationEdge> edges;
    std::set<Variable> constants;
    std::set<Variable> defined;

    static ImpactRelationGraph top();
    void forget(Variable variable);
    void setConstant(Variable variable, bool strong = true);
    void setVariable(Variable target, Variable source, bool strong = true);
    void assume(Variable lhs, Variable rhs);
    bool finiteDifference(Variable lhs, Variable rhs) const;
    bool finiteUpper(Variable variable) const;
    bool finiteLower(Variable variable) const;
    ImpactRelationGraph join(const ImpactRelationGraph& other) const;
    /// Sparrow AbsOct.le: defined grows, edges shrink, constants shrink.
    bool le(const ImpactRelationGraph& other) const;
    /// Sparrow AbsOct.check: a directed edge path source -> target.
    bool hasPath(Variable source, Variable target) const;
    bool operator==(const ImpactRelationGraph& other) const;
    bool operator!=(const ImpactRelationGraph& other) const { return !(*this == other); }
};
}
#endif
