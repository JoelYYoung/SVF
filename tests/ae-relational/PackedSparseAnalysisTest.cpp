// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AE/Core/PackedSparseAnalysis.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>

namespace A = SVF::AbstractDomain;
namespace P = A::PackedSparse;
using E = A::LinearExpression;
using O = P::Operation;
const A::Variable x(1), y(2), o(3), z(4);
E c(long n) { return E(A::Rational(n)); }
A::Interval interval(long l, long h) { return A::Interval::closed(A::Rational(l), A::Rational(h)); }
void check(bool b, const char* m) { if (!b) throw std::runtime_error(m); }
struct Fixture
{
    P::Graph graph{{{}, O::entry()}};
    std::vector<std::pair<std::size_t, P::Verdict>> queries;
    std::size_t add(O op, std::vector<std::size_t> pred = {})
    {
        if (pred.empty()) pred.push_back(graph.size() - 1);
        graph.push_back({std::move(pred), std::move(op)}); return graph.size() - 1;
    }
    void query(A::LinearConstraint c, P::Verdict v)
    { queries.emplace_back(add(O::query(c)), v); }
};
void run(const std::string& name, const P::Layout& layout, const Fixture& f)
{
    auto deps = P::buildDependencies(layout, f.graph);
    auto d = P::solveDense(layout, f.graph), s = P::solveSparse(layout, f.graph, deps);
    P::verify(layout, f.graph, d); P::verify(layout, f.graph, s);
    P::compare(layout, f.graph, d, s);
    for (auto q : f.queries)
    {
        check(P::query(layout, f.graph, d, q.first) == q.second, "Dense query expectation failed");
        check(P::query(layout, f.graph, s, q.first) == q.second, "Sparse query expectation failed");
    }
    std::cout << name << '\t' << layout.packs.size() << '\t' << f.graph.size() << '\t'
              << d.slots << '\t' << s.slots << '\t' << deps.edges << '\t'
              << d.rounds << '\t' << s.rounds << "\tPASS\n";
}
Fixture synthetic(std::size_t count)
{
    Fixture f;
    for (std::size_t i = 0; i < count; ++i)
    {
        A::Variable a(2*i+1), b(2*i+2);
        f.add(O::havoc(a, interval(0, 10)));
        f.add(O::assign(b, E(a)+c(1)));
        f.add(O{}); // unrelated transfer location, not a synthetic numerical def
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        A::Variable a(2*i+1), b(2*i+2);
        f.add(O::assume(A::lessEqual(E(a), c(5))));
        f.add(O{});
        f.query(A::lessEqual(E(b), c(6)), P::Verdict::Safe);
    }
    return f;
}
P::Layout syntheticLayout(std::size_t count)
{
    P::Pack variables;
    std::vector<P::Pack> candidates;
    for (std::size_t i = 0; i < count; ++i)
    {
        A::Variable a(2*i+1), b(2*i+2);
        variables.insert(a); variables.insert(b); candidates.push_back({a,b});
    }
    return P::Layout(variables, candidates);
}
double seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b)
{ return std::chrono::duration<double>(b-a).count(); }
long rssKiB()
{
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error("getrusage failed");
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
}
void bench(std::size_t count, const std::string& mode)
{
    check(count > 0 && count <= 256, "benchmark count must be in 1..256");
    auto begin = std::chrono::steady_clock::now();
    auto layout = syntheticLayout(count);
    auto f = synthetic(count);
    auto prepared = std::chrono::steady_clock::now();
    P::DependencyGraph deps;
    check(mode == "dense" || mode == "sparse", "invalid benchmark mode");
    if (mode == "sparse") deps = P::buildDependencies(layout, f.graph);
    auto built = std::chrono::steady_clock::now();
    auto result = mode == "dense" ? P::solveDense(layout, f.graph)
                                    : P::solveSparse(layout, f.graph, deps);
    auto solved = std::chrono::steady_clock::now();
    auto solveRss = rssKiB();
    P::verify(layout, f.graph, result);
    for (auto q : f.queries) check(P::query(layout, f.graph, result, q.first) == q.second,
                                  "benchmark query failed");
    auto checked = std::chrono::steady_clock::now();
    std::cout << "mode,packs,variables,nodes,slots,edges,rounds,queries,safe,prepare_s,dependency_s,solve_s,check_s,total_s,solve_peak_kib,total_peak_kib\n"
              << mode << ',' << count << ',' << 2*count << ',' << f.graph.size() << ','
              << result.slots << ',' << deps.edges << ',' << result.rounds << ','
              << f.queries.size() << ',' << f.queries.size() << ',' << seconds(begin,prepared) << ','
              << seconds(prepared,built) << ',' << seconds(built,solved) << ','
              << seconds(solved,checked) << ',' << seconds(begin,checked) << ','
              << solveRss << ',' << rssKiB() << '\n';
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 4 && std::string(argv[1]) == "--bench")
        { bench(std::stoul(argv[2]), argv[3]); return 0; }
        check(argc == 1 || (argc == 2 && std::string(argv[1]) == "--mutation"), "invalid arguments");
        P::Layout layout({x,y,o,z}, {{x,y,o},{z}});
        std::cout << "case\tpacks\tnodes\tdense_slots\tsparse_slots\tedges\tdense_rounds\tsparse_rounds\tcheck\n";
        {
            Fixture f;
            f.add(O::assign(o,c(1))); f.add(O::assign(x,E(o)));
            f.add(O::assign(o,c(2))); f.add(O::assign(y,E(o)));
            f.query(A::equal(E(y)-E(x),c(1)),P::Verdict::Safe);
            run("load_store_load_constants",layout,f);
        }
        for (bool overwrite : {false,true})
        {
            Fixture f;
            f.add(O::havoc(o,interval(1,10))); f.add(O::assign(x,E(o)));
            if (overwrite) f.add(O::havoc(o,interval(2,10)));
            else f.add(O::assume(A::greaterEqual(E(o),c(2))));
            f.add(O::assign(y,E(o)));
            f.query(A::equal(E(x),E(y)),overwrite?P::Verdict::May:P::Verdict::Safe);
            run(overwrite?"overwrite_kills_stale_relation":"assume_preserves_memory_identity",layout,f);
        }
        {
            Fixture f;
            f.add(O::havoc(x,interval(0,10))); f.add(O::assign(y,E(x)+c(1)));
            f.add(O{}); f.add(O::assume(A::lessEqual(E(x),c(5))));
            f.query(A::lessEqual(E(y),c(6)),P::Verdict::Safe);
            run("assume_indirect_def",layout,f);
            if (argc == 2)
            {
                auto deps = P::buildDependencies(layout,f.graph);
                // Bypass the assume's pack definition at the query. The
                // original-equation checker must catch the corrupted route.
                auto pack = *layout.containing(y).begin();
                deps.incoming[5][pack] = {2};
                auto mutated = P::solveSparse(layout,f.graph,deps);
                P::verify(layout,f.graph,mutated);
                throw std::runtime_error("mutation was not detected");
            }
        }
        {
            Fixture f;
            f.add(O::havoc(x,interval(0,10))); f.add(O::havoc(y,interval(0,10)));
            f.add(O::assign(o,E(x))); f.add(O::assume(A::equal(E(o),E(y))));
            f.add(O::havoc(o,interval(2,10)));
            f.query(A::equal(E(x),E(y)),P::Verdict::Safe);
            f.query(A::equal(E(x),E(o)),P::Verdict::May);
            run("assume_scalar_fact_survives_object_write",layout,f);
        }
        for (bool both : {false,true})
        {
            Fixture f;
            f.add(O::havoc(x,interval(0,10))); auto b=f.add(O::havoc(y,interval(0,10)));
            auto left=f.add(O::assume(A::equal(E(x),E(y))),{b});
            auto right=f.add(both?O::assume(A::equal(E(x),E(y))):O{}, {b});
            f.add(O{}, {left,right});
            f.query(A::equal(E(x),E(y)),both?P::Verdict::Safe:P::Verdict::May);
            run(both?"both_branches_refine":"one_branch_no_leak",layout,f);
        }
        {
            Fixture f;
            f.add(O::assign(x,c(0))); f.add(O::assign(y,c(2))); f.add(O::assign(o,E(x)));
            f.add(O::assign(o,c(7),true));
            f.query(A::equal(E(y)-E(x),c(2)),P::Verdict::Safe);
            f.query(A::equal(E(o),E(x)),P::Verdict::May);
            run("weak_update_reads_old_pack",layout,f);
        }
        {
            Fixture f;
            f.add(O::assign(x,c(0))); f.add(O::assign(y,c(2)));
            auto head=f.add(O{});
            f.add(O::assume(A::lessEqual(E(x),c(2))));
            f.add(O::assign(x,E(x)+c(1))); auto back=f.add(O::assign(y,E(y)+c(1)));
            f.graph[head].predecessors.push_back(back);
            f.add(O::assume(A::greaterEqual(E(x),c(3))),{head});
            f.query(A::equal(E(y)-E(x),c(2)),P::Verdict::Safe);
            run("bounded_loop",layout,f);
        }
        {
            P::Layout overlap({x,y,o,z},{{x,o},{y,o},{z}});
            Fixture f;
            f.add(O::havoc(x,interval(1,2))); f.add(O::assign(y,E(x)));
            f.query(A::lessEqual(E(y),c(2)),P::Verdict::Safe);
            f.query(A::equal(E(x),E(y)),P::Verdict::May);
            run("cross_pack_interval_not_equality",overlap,f);
        }
        {
            P::Layout overlap({x,y,o,z},{{x,y},{y,o},{z}});
            Fixture f;
            f.add(O::havoc(x,interval(0,10))); f.add(O::assign(y,E(x)+c(1)));
            f.add(O::assume(A::lessEqual(E(x),c(5))));
            f.add(O::assign(o,E(y)));
            f.query(A::lessEqual(E(o),c(6)),P::Verdict::May);
            f.add(O::assign(z,E(y)));
            f.query(A::lessEqual(E(z),c(6)),P::Verdict::Safe);
            run("overlap_projection_reads_all_owners",overlap,f);
        }
        {
            Fixture f;
            f.add(O::assume(A::equal(c(0),c(1))));
            f.query(A::equal(E(x),c(7)),P::Verdict::Unreachable);
            run("constant_false_guard",layout,f);
        }
        {
            Fixture f;
            auto unreachable=f.add(O::assign(x,c(99)),{2});
            f.graph[unreachable].predecessors={unreachable}; // no entry path
            f.query(A::equal(E(x),c(99)),P::Verdict::Unreachable);
            run("unreachable_cycle_no_entry_seed",layout,f);
        }
        {
            P::Pack vars;
            for (unsigned i=1;i<=23;++i) vars.insert(A::Variable(i));
            P::Layout split(vars,{vars});
            check(split.packs.size()==3,"split count");
            for (const auto& pack:split.packs) check(pack.size()<=10,"split cap");
            for (auto v:vars) check(!split.containing(v).empty(),"uncovered variable");
            std::cout<<"deterministic_split_cap_10\tPASS\n";
        }
        for (std::size_t count : {1,4,16,64}) run("synthetic_"+std::to_string(count),syntheticLayout(count),synthetic(count));
        {
            Fixture f;
            auto init=f.add(O::assign(x,c(0)));
            auto loop=f.add(O::assign(x,E(x)+c(1)));
            f.graph[loop].predecessors={init,loop};
            const auto deps=P::buildDependencies(layout,f.graph);
            for (bool sparse : {false,true})
            {
                bool rejected=false;
                try
                {
                    if (sparse) (void)P::solveSparse(layout,f.graph,deps,3);
                    else (void)P::solveDense(layout,f.graph,3);
                }
                catch(const std::runtime_error& e)
                { rejected=std::string(e.what()).find("did not converge")!=std::string::npos; }
                check(rejected,"iteration exhaustion must not produce a result");
            }
            std::cout<<"nonconvergence_rejected\tPASS\n";
        }
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
