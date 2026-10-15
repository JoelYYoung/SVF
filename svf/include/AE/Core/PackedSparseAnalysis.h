// Experimental Oh et al. PLDI 2012 pack-level sparse numerical analysis.
// Mathematical integers and explicit memory cells; frontend integration is separate.
// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_AE_PACKED_SPARSE_ANALYSIS_H
#define SVF_AE_PACKED_SPARSE_ANALYSIS_H

#include "AE/Core/OctagonDomain.h"
#include <map>
#include <set>
#include <vector>

namespace SVF::AbstractDomain::PackedSparse
{
using Pack = std::set<Variable>;
using PackIDs = std::set<std::size_t>;

// Fixed during solving. Overlap is permitted. Candidate groups come from a
// frontend; sorted consecutive chunks are our deterministic split policy.
struct Layout
{
    std::vector<Pack> packs;
    std::map<Variable, PackIDs> owners;
    Layout(const Pack& variables, const std::vector<Pack>& candidates,
           std::size_t maximumSize = 10);
    const PackIDs& containing(Variable variable) const;
};

enum class Kind { Entry, Identity, Assign, WeakAssign, Havoc, Assume, Query };
struct Operation
{
    Kind kind = Kind::Identity;
    Variable target{0};
    LinearExpression expression;
    ConstraintKind relation = ConstraintKind::Equal;
    Interval range = Interval::top();
    static Operation entry();
    static Operation assign(Variable target, LinearExpression value, bool weak = false);
    static Operation havoc(Variable target, Interval range = Interval::top());
    static Operation assume(const LinearConstraint& constraint);
    static Operation query(const LinearConstraint& constraint);
};
struct Node
{
    std::vector<std::size_t> predecessors;
    Operation operation;
};
using Graph = std::vector<Node>;
struct Access { PackIDs definitions, uses; };
std::vector<Access> deriveAccess(const Layout& layout, const Graph& graph);

// Routes use every approximate definition as a kill, including weak updates
// and assume. Their old value is read through Uses (Definition 5 in the paper).
using Sources = std::map<std::size_t, std::vector<std::size_t>>;
struct DependencyGraph
{
    std::vector<Access> access;
    std::vector<Sources> incoming;
    std::size_t edges = 0;
};
DependencyGraph buildDependencies(const Layout& layout, const Graph& graph);

struct Result
{
    // Dense: every pack at every node. Sparse: only declared definitions.
    std::vector<std::map<std::size_t, OctagonDomain>> states;
    std::size_t rounds = 0;
    std::size_t slots = 0;
    std::size_t transfers = 0;
};
Result solveDense(const Layout& layout, const Graph& graph,
                  std::size_t maximumRounds = 1000);
Result solveSparse(const Layout& layout, const Graph& graph,
                   const DependencyGraph& dependencies,
                   std::size_t maximumRounds = 1000);

// Independent CFG reconstruction and original packed equations. Throws on
// missing/extra slots, graph-contract errors, nonconvergence, or mismatch.
void verify(const Layout& layout, const Graph& graph, const Result& result);
void compare(const Layout& layout, const Graph& graph,
             const Result& dense, const Result& sparse);
// Query results deliberately distinguish product inconsistency from a proof.
enum class Verdict { Safe, May, Unreachable };
Verdict query(const Layout& layout, const Graph& graph,
              const Result& result, std::size_t node);
} // namespace SVF::AbstractDomain::PackedSparse
#endif
