// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AE/Core/PackedSparseAnalysis.h"
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace SVF::AbstractDomain::PackedSparse
{
namespace
{
void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
OctagonConfig config()
{
    OctagonConfig c;
    c.storage = OctagonStorageKind::ComponentDense;
    return c;
}
OctagonDomain bottom() { return OctagonDomain::bottom(config()); }
using Read = std::function<OctagonDomain(std::size_t)>;
void include(PackIDs& into, const PackIDs& from)
{
    into.insert(from.begin(), from.end());
}
Pack variables(const LinearExpression& expression)
{
    Pack result;
    for (const auto& term : expression.terms()) result.insert(term.first);
    return result;
}
void restrictRange(OctagonDomain& state, Variable variable, const Interval& range)
{
    if (range.isBottom()) { state = bottom(); return; }
    const LinearExpression v(variable);
    if (range.lower().isFinite())
        state.assume(LinearConstraint(v - LinearExpression(range.lower().value()),
            range.lower().isStrict() ? ConstraintKind::GreaterThan : ConstraintKind::GreaterEqual));
    if (range.upper().isFinite())
        state.assume(LinearConstraint(v - LinearExpression(range.upper().value()),
            range.upper().isStrict() ? ConstraintKind::LessThan : ConstraintKind::LessEqual));
}
Interval projection(const Layout& layout, Variable variable, const Read& read)
{
    auto range = Interval::top();
    for (auto p : layout.containing(variable)) range.meetWith(read(p).bound(variable));
    return range;
}
OctagonDomain transfer(const Layout& layout, const Operation& op,
                        std::size_t p, const Read& read)
{
    if (op.kind == Kind::Entry) return OctagonDomain::top(config());
    auto state = read(p);
    if (state.isBottom() || op.kind == Kind::Identity || op.kind == Kind::Query)
        return state;
    if (op.kind == Kind::Havoc)
    {
        state.forget(op.target);
        restrictRange(state, op.target, op.range);
        return state;
    }
    // Embed outside variables as independent interval temporaries, execute,
    // then existentially project them away. Every imported value is read from
    // the same pre-state, including overlapping target packs.
    auto old = state;
    for (auto variable : variables(op.expression))
        if (!layout.packs.at(p).count(variable))
            restrictRange(state, variable, projection(layout, variable, read));
    if (op.kind == Kind::Assume)
        state.assume(LinearConstraint(op.expression, op.relation));
    else
        state.assign(op.target, op.expression);
    const auto& pack = layout.packs.at(p);
    state.project(std::vector<Variable>(pack.begin(), pack.end()));
    if (op.kind == Kind::WeakAssign) state.joinWith(old);
    return state;
}
void validate(const Layout& layout, const Graph& graph)
{
    check(!graph.empty() && graph[0].operation.kind == Kind::Entry,
          "node zero must be entry");
    check(graph[0].predecessors.empty(), "entry cannot have predecessors");
    check(!layout.packs.empty(), "empty numerical vocabulary");
    for (std::size_t n = 0; n < graph.size(); ++n)
    {
        check(n == 0 || graph[n].operation.kind != Kind::Entry, "multiple entries");
        for (auto p : graph[n].predecessors) check(p < graph.size(), "invalid predecessor");
        for (auto v : variables(graph[n].operation.expression)) (void)layout.containing(v);
    }
}
// Pre-state or post-state reaching definitions, derived from the original CFG.
// A visited node breaks a cycle; every path to an entry/definition is explored.
std::vector<std::size_t> reaching(const Graph& graph, const std::vector<Access>& access,
                                  std::size_t n, std::size_t p, bool post)
{
    std::set<std::size_t> found, seen;
    std::vector<std::size_t> pending = post ? std::vector<std::size_t>{n}
                                           : graph[n].predecessors;
    while (!pending.empty())
    {
        auto current = pending.back(); pending.pop_back();
        if (!seen.insert(current).second) continue;
        if (access[current].definitions.count(p)) found.insert(current);
        else pending.insert(pending.end(), graph[current].predecessors.begin(),
                            graph[current].predecessors.end());
    }
    return {found.begin(), found.end()};
}
OctagonDomain merged(const Result& result, const std::vector<std::size_t>& sources,
                      std::size_t p)
{
    auto state = bottom();
    for (auto n : sources)
    {
        const auto& slots = result.states.at(n);
        auto it = slots.find(p);
        check(it != slots.end(), "missing source pack slot");
        state.joinWith(it->second);
    }
    return state;
}
std::vector<std::vector<OctagonDomain>> expand(const Layout& layout, const Graph& graph,
                                              const Result& result)
{
    check(result.states.size() == graph.size(), "wrong result graph size");
    const auto access = deriveAccess(layout, graph);
    std::vector<std::vector<OctagonDomain>> full;
    bool missing = false;
    for (std::size_t n = 0; n < graph.size(); ++n)
    {
        full.emplace_back();
        for (std::size_t p = 0; p < layout.packs.size(); ++p)
        {
            auto it = result.states[n].find(p);
            if (it != result.states[n].end()) full.back().push_back(it->second);
            else
            {
                check(!access[n].definitions.count(p), "missing defined pack");
                full.back().push_back(bottom());
                missing = true;
            }
        }
        for (const auto& slot : result.states[n]) check(slot.first < layout.packs.size(), "invalid pack slot");
    }
    if (!missing) return full;
    // Independently solve ONLY the omitted identity equations on the CFG.
    // Defined slots are boundary constants from the submitted result. This
    // avoids a backward path search for every (node, pack), without consulting
    // the solver's compiled routes. Even cyclic CFGs need at most |nodes|
    // propagation rounds: paths between fixed boundaries have that length
    // after removal of repeated nodes. The extra round checks stability.
    for (std::size_t round = 0; round <= graph.size(); ++round)
    {
        bool changed = false;
        for (std::size_t n = 0; n < graph.size(); ++n)
            for (std::size_t p = 0; p < layout.packs.size(); ++p)
                if (!result.states[n].count(p))
                {
                    auto next = bottom();
                    for (auto pred : graph[n].predecessors) next.joinWith(full[pred][p]);
                    if (next.isEquivalentTo(full[n][p]) != CheckResult::True)
                    { full[n][p] = std::move(next); changed = true; }
                }
        if (!changed) return full;
    }
    throw std::runtime_error("CFG identity reconstruction did not converge");
}
Result solve(const Layout& layout, const Graph& graph, const DependencyGraph* deps,
             std::size_t maximumRounds)
{
    validate(layout, graph);
    const auto derived = deriveAccess(layout, graph);
    const auto& access = deps ? deps->access : derived;
    if (deps)
    {
        check(access.size() == graph.size() && deps->incoming.size() == graph.size(), "wrong dependency size");
        for (std::size_t n = 0; n < graph.size(); ++n)
            check(access[n].definitions == derived[n].definitions && access[n].uses == derived[n].uses,
                  "dependency access contract mismatch");
    }
    Result result;
    result.states.resize(graph.size());
    for (std::size_t n = 0; n < graph.size(); ++n)
        for (std::size_t p = 0; p < layout.packs.size(); ++p)
            if (!deps || access[n].definitions.count(p))
            { result.states[n].emplace(p, bottom()); ++result.slots; }
    for (std::size_t round = 1; round <= maximumRounds; ++round)
    {
        bool changed = false;
        for (std::size_t n = 0; n < graph.size(); ++n)
        {
            // Snapshot all reads before updating ANY output (self-loop included).
            std::map<std::size_t, OctagonDomain> input;
            if (deps)
                for (auto p : access[n].uses)
                {
                    auto route = deps->incoming[n].find(p);
                    check(route != deps->incoming[n].end(), "missing use route");
                    input.emplace(p, merged(result, route->second, p));
                }
            else
                for (std::size_t p = 0; p < layout.packs.size(); ++p)
                    input.emplace(p, merged(result, graph[n].predecessors, p));
            Read read = [&](std::size_t p)
            {
                check(access[n].uses.count(p), "undeclared implicit pack read");
                return input.at(p);
            };
            for (auto& slot : result.states[n])
            {
                const auto p = slot.first;
                auto next = access[n].definitions.count(p)
                    ? transfer(layout, graph[n].operation, p, read) : input.at(p);
                if (access[n].definitions.count(p)) ++result.transfers;
                if (slot.second.isEquivalentTo(next) != CheckResult::True)
                { slot.second = std::move(next); changed = true; }
            }
        }
        result.rounds = round;
        if (!changed) return result;
    }
    throw std::runtime_error("packed solver did not converge within maximumRounds");
}
} // namespace

Layout::Layout(const Pack& vars, const std::vector<Pack>& candidates, std::size_t cap)
{
    check(cap > 0, "maximum pack size must be positive");
    for (auto v : vars) check(v.type().kind == NumericKind::Integer, "kernel requires mathematical integers");
    std::set<Pack> unique;
    for (const auto& candidate : candidates)
    {
        Pack chunk;
        for (auto v : candidate)
        {
            check(vars.count(v), "candidate variable outside vocabulary");
            chunk.insert(v);
            if (chunk.size() == cap) { unique.insert(chunk); chunk.clear(); }
        }
        if (!chunk.empty()) unique.insert(chunk);
    }
    Pack covered;
    for (const auto& p : unique) covered.insert(p.begin(), p.end());
    for (auto v : vars) if (!covered.count(v)) unique.insert({v});
    packs.assign(unique.begin(), unique.end());
    for (std::size_t p = 0; p < packs.size(); ++p)
        for (auto v : packs[p]) owners[v].insert(p);
}
const PackIDs& Layout::containing(Variable v) const
{
    auto it = owners.find(v);
    check(it != owners.end(), "variable outside packing vocabulary");
    return it->second;
}
Operation Operation::entry() { Operation op; op.kind = Kind::Entry; return op; }
Operation Operation::assign(Variable v, LinearExpression e, bool weak)
{
    Operation op; op.kind = weak ? Kind::WeakAssign : Kind::Assign;
    op.target = v; op.expression = std::move(e); return op;
}
Operation Operation::havoc(Variable v, Interval range)
{
    Operation op; op.kind = Kind::Havoc; op.target = v; op.range = std::move(range); return op;
}
Operation Operation::assume(const LinearConstraint& c)
{
    Operation op; op.kind = Kind::Assume; op.expression = c.expression(); op.relation = c.kind(); return op;
}
Operation Operation::query(const LinearConstraint& c)
{
    auto op = assume(c); op.kind = Kind::Query; return op;
}
std::vector<Access> deriveAccess(const Layout& layout, const Graph& graph)
{
    validate(layout, graph);
    std::vector<Access> access(graph.size());
    for (std::size_t n = 0; n < graph.size(); ++n)
    {
        auto& a = access[n];
        const auto& op = graph[n].operation;
        if (op.kind == Kind::Entry)
        { for (std::size_t p = 0; p < layout.packs.size(); ++p) a.definitions.insert(p); continue; }
        if (op.kind == Kind::Assign || op.kind == Kind::WeakAssign || op.kind == Kind::Havoc)
            include(a.definitions, layout.containing(op.target));
        if (op.kind == Kind::Assume || op.kind == Kind::Query)
        {
            for (auto v : variables(op.expression)) include(a.definitions, layout.containing(v));
            // A constant false assume affects every pack; constant queries must
            // still read state to distinguish inconsistency from a proof.
            if (op.expression.terms().empty())
                for (std::size_t p = 0; p < layout.packs.size(); ++p) a.definitions.insert(p);
        }
        a.uses = a.definitions;
        if (op.kind == Kind::Assign || op.kind == Kind::WeakAssign || op.kind == Kind::Assume)
            for (auto v : variables(op.expression)) include(a.uses, layout.containing(v));
    }
    return access;
}
DependencyGraph buildDependencies(const Layout& layout, const Graph& graph)
{
    DependencyGraph deps;
    deps.access = deriveAccess(layout, graph);
    deps.incoming.resize(graph.size());
    for (std::size_t n = 0; n < graph.size(); ++n)
        for (auto p : deps.access[n].uses)
        {
            auto sources = reaching(graph, deps.access, n, p, false);
            deps.edges += sources.size();
            deps.incoming[n].emplace(p, std::move(sources));
        }
    return deps;
}
Result solveDense(const Layout& l, const Graph& g, std::size_t limit)
{ return solve(l, g, nullptr, limit); }
Result solveSparse(const Layout& l, const Graph& g, const DependencyGraph& d, std::size_t limit)
{ return solve(l, g, &d, limit); }
void verify(const Layout& layout, const Graph& graph, const Result& result)
{
    auto full = expand(layout, graph, result);
    auto access = deriveAccess(layout, graph);
    for (std::size_t n = 0; n < graph.size(); ++n)
    {
        Read read = [&](std::size_t p)
        {
            auto input = bottom();
            for (auto pred : graph[n].predecessors) input.joinWith(full[pred][p]);
            return input;
        };
        for (std::size_t p = 0; p < layout.packs.size(); ++p)
        {
            auto next = access[n].definitions.count(p)
                ? transfer(layout, graph[n].operation, p, read) : read(p);
            check(next.isEquivalentTo(full[n][p]) == CheckResult::True,
                  "original packed equation mismatch");
        }
    }
}
void compare(const Layout& layout, const Graph& graph, const Result& dense, const Result& sparse)
{
    auto d = expand(layout, graph, dense), s = expand(layout, graph, sparse);
    for (std::size_t n = 0; n < graph.size(); ++n)
        for (std::size_t p = 0; p < layout.packs.size(); ++p)
            check(d[n][p].isEquivalentTo(s[n][p]) == CheckResult::True, "dense/sparse pack mismatch");
}
Verdict query(const Layout& layout, const Graph& graph, const Result& result, std::size_t n)
{
    const auto& op = graph.at(n).operation;
    check(op.kind == Kind::Query, "query requires explicit query node");
    const auto access = deriveAccess(layout, graph);
    // Query is an explicit identity-definition of each observed pack, hence
    // both modes already have all its declared reads at this exact node.
    Read read = [&](std::size_t p) { return result.states.at(n).at(p); };
    bool safe = false;
    for (auto p : access[n].uses)
    {
        auto state = read(p);
        if (state.isBottom()) return Verdict::Unreachable;
        if (state.entails(LinearConstraint(op.expression, op.relation)) == CheckResult::True) safe = true;
    }
    // Extra sound, nonrelational query fallback using the declared pack reads.
    auto intervals = OctagonDomain::top(config());
    for (auto v : variables(op.expression)) restrictRange(intervals, v, projection(layout, v, read));
    if (intervals.isBottom()) return Verdict::Unreachable;
    if (intervals.entails(LinearConstraint(op.expression, op.relation)) == CheckResult::True) safe = true;
    return safe ? Verdict::Safe : Verdict::May;
}
} // namespace SVF::AbstractDomain::PackedSparse
