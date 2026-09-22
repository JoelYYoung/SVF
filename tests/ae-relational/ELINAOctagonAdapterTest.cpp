#include "AE/Core/ElinaOctagonDomain.h"
#include "AE/Core/NumericalDomainFactory.h"

#include <cfenv>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace AD = SVF::AbstractDomain;

namespace
{

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "FAIL: " << message << '\n';
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, const std::string& message)
{
    if (!condition)
        fail(message);
}

void expectSingleton(const AD::Interval& interval, int expected,
                     const std::string& message)
{
    expect(interval.isSingleton() &&
               interval.singletonValue() == AD::Rational(expected),
           message);
}

struct Stage
{
    std::string name;
    AD::Interval x;
    AD::Interval y;
    AD::Interval z;
    AD::CheckResult relation = AD::CheckResult::Unknown;
    bool top = false;
    bool bottom = false;
};

struct SequenceResult
{
    std::vector<Stage> stages;
};

const AD::Variable X(1);
const AD::Variable Y(2);
const AD::Variable Z(3);

template <typename Domain>
void capture(SequenceResult& result, const std::string& name,
             const Domain& state, const AD::LinearConstraint& relation,
             int expectedRounding)
{
    expect(std::fegetround() == expectedRounding,
           name + " leaked ELINA's FPU rounding mode");
    result.stages.push_back({name, state.bound(X), state.bound(Y),
                             state.bound(Z), state.entails(relation),
                             state.isTop(), state.isBottom()});
    expect(std::fegetround() == expectedRounding,
           name + " query leaked ELINA's FPU rounding mode");
}

template <typename Domain>
SequenceResult runFiniteSequence(int expectedRounding)
{
    SequenceResult result;
    const AD::LinearConstraint yEqualsXPlusOne = AD::equal(
        AD::LinearExpression(Y),
        AD::LinearExpression(X) + AD::LinearExpression(AD::Rational(1)));
    const AD::LinearConstraint xEqualsYPlusOne = AD::equal(
        AD::LinearExpression(X),
        AD::LinearExpression(Y) + AD::LinearExpression(AD::Rational(1)));
    const AD::LinearConstraint zEqualsXPlusOne = AD::equal(
        AD::LinearExpression(Z),
        AD::LinearExpression(X) + AD::LinearExpression(AD::Rational(1)));

    Domain top = Domain::top();
    capture(result, "top", top, yEqualsXPlusOne, expectedRounding);

    Domain bounded = Domain::top();
    bounded.assume(AD::greaterEqual(AD::LinearExpression(X),
                                    AD::LinearExpression(AD::Rational(0))));
    bounded.assume(AD::lessEqual(AD::LinearExpression(X),
                                 AD::LinearExpression(AD::Rational(8))));
    bounded.assign(Y, AD::LinearExpression(X) +
                          AD::LinearExpression(AD::Rational(1)));
    capture(result, "assign-assume", bounded, yEqualsXPlusOne,
            expectedRounding);

    bounded.assignParallel({
        {X, AD::LinearExpression(Y)},
        {Y, AD::LinearExpression(X)}
    });
    capture(result, "parallel-assign", bounded, xEqualsYPlusOne,
            expectedRounding);

    Domain modified(bounded);
    modified.assign(Z, AD::LinearExpression(X) +
                           AD::LinearExpression(AD::Rational(1)));
    modified.assume(AD::lessEqual(AD::LinearExpression(Z),
                                  AD::LinearExpression(AD::Rational(5))));
    capture(result, "copy-modified", modified, zEqualsXPlusOne,
            expectedRounding);
    capture(result, "copy-original", bounded, xEqualsYPlusOne,
            expectedRounding);

    Domain first = Domain::fromConstraints({
        AD::equal(AD::LinearExpression(X),
                  AD::LinearExpression(AD::Rational(0))),
        AD::equal(AD::LinearExpression(Y),
                  AD::LinearExpression(AD::Rational(1)))
    });
    Domain second = Domain::fromConstraints({
        AD::equal(AD::LinearExpression(X),
                  AD::LinearExpression(AD::Rational(2))),
        AD::equal(AD::LinearExpression(Y),
                  AD::LinearExpression(AD::Rational(3)))
    });
    Domain joined = first.join(second);
    capture(result, "join", joined, yEqualsXPlusOne, expectedRounding);

    Domain broad = Domain::top();
    broad.assume(AD::greaterEqual(AD::LinearExpression(X),
                                  AD::LinearExpression(AD::Rational(0))));
    broad.assume(AD::lessEqual(AD::LinearExpression(X),
                               AD::LinearExpression(AD::Rational(10))));
    broad.assign(Y, AD::LinearExpression(X) +
                        AD::LinearExpression(AD::Rational(1)));
    Domain limiter = Domain::top();
    limiter.assume(AD::greaterEqual(AD::LinearExpression(X),
                                    AD::LinearExpression(AD::Rational(3))));
    limiter.assume(AD::lessEqual(AD::LinearExpression(X),
                                 AD::LinearExpression(AD::Rational(7))));
    Domain met = broad.meet(limiter);
    capture(result, "meet", met, yEqualsXPlusOne, expectedRounding);

    Domain forgotten(joined);
    forgotten.forget(X);
    capture(result, "forget", forgotten, yEqualsXPlusOne,
            expectedRounding);

    Domain projected(met);
    projected.project({Y});
    capture(result, "project", projected, yEqualsXPlusOne,
            expectedRounding);

    Domain preimage = Domain::fromConstraints({AD::equal(
        AD::LinearExpression(X),
        AD::LinearExpression(AD::Rational(5))) });
    preimage.substitute(X, AD::LinearExpression(Y) +
                              AD::LinearExpression(AD::Rational(1)));
    capture(result, "substitute", preimage,
            AD::equal(AD::LinearExpression(Y),
                      AD::LinearExpression(AD::Rational(4))),
            expectedRounding);

    Domain strict = Domain::top();
    strict.assume(AD::greaterThan(AD::LinearExpression(X),
                                  AD::LinearExpression(AD::Rational(0))));
    strict.assume(AD::lessEqual(AD::LinearExpression(X),
                                AD::LinearExpression(AD::Rational(1))));
    capture(result, "integer-strict", strict,
            AD::equal(AD::LinearExpression(X),
                      AD::LinearExpression(AD::Rational(1))),
            expectedRounding);

    Domain current = Domain::fromConstraints({AD::equal(
        AD::LinearExpression(X),
        AD::LinearExpression(AD::Rational(0))) });
    Domain next = Domain::top();
    next.assume(AD::greaterEqual(AD::LinearExpression(X),
                                 AD::LinearExpression(AD::Rational(0))));
    next.assume(AD::lessEqual(AD::LinearExpression(X),
                              AD::LinearExpression(AD::Rational(1))));
    Domain widened = current.widen(next);
    capture(result, "widen", widened,
            AD::greaterEqual(AD::LinearExpression(X),
                             AD::LinearExpression(AD::Rational(0))),
            expectedRounding);

    Domain bottom = Domain::bottom();
    capture(result, "bottom", bottom, yEqualsXPlusOne, expectedRounding);
    return result;
}

void checkNativeOracle(const SequenceResult& result)
{
    expect(result.stages.size() == 13, "finite sequence stage count");
    const auto& top = result.stages[0];
    expect(top.top && !top.bottom && top.x.isTop(), "top contract");

    const auto& assigned = result.stages[1];
    expect(assigned.x == AD::Interval::closed(AD::Rational(0),
                                               AD::Rational(8)),
           "assign/assume x bound");
    expect(assigned.y == AD::Interval::closed(AD::Rational(1),
                                               AD::Rational(9)),
           "assign/assume y bound");
    expect(assigned.relation == AD::CheckResult::True,
           "assign/assume relation");

    const auto& parallel = result.stages[2];
    expect(parallel.x == AD::Interval::closed(AD::Rational(1),
                                               AD::Rational(9)) &&
               parallel.y == AD::Interval::closed(AD::Rational(0),
                                                   AD::Rational(8)) &&
               parallel.relation == AD::CheckResult::True,
           "parallel assignment simultaneous semantics");

    const auto& modified = result.stages[3];
    expect(modified.z == AD::Interval::closed(AD::Rational(2),
                                               AD::Rational(5)) &&
               modified.relation == AD::CheckResult::True,
           "copy modification semantics");
    expect(result.stages[4].z.isTop(), "copy changed the source state");

    const auto& joined = result.stages[5];
    expect(joined.x == AD::Interval::closed(AD::Rational(0),
                                             AD::Rational(2)) &&
               joined.y == AD::Interval::closed(AD::Rational(1),
                                                 AD::Rational(3)) &&
               joined.relation == AD::CheckResult::True,
           "join contract");

    const auto& met = result.stages[6];
    expect(met.x == AD::Interval::closed(AD::Rational(3), AD::Rational(7)) &&
               met.y == AD::Interval::closed(AD::Rational(4),
                                              AD::Rational(8)) &&
               met.relation == AD::CheckResult::True,
           "meet contract");
    expect(result.stages[7].x.isTop() && !result.stages[7].y.isTop(),
           "forget contract");
    expect(result.stages[8].x.isTop() &&
               result.stages[8].y == AD::Interval::closed(
                   AD::Rational(4), AD::Rational(8)),
           "project contract");
    expectSingleton(result.stages[9].y, 4, "substitution preimage");
    expectSingleton(result.stages[10].x, 1, "integer strict rewrite");
    expect(result.stages[10].relation == AD::CheckResult::True,
           "integer strict entailment");
    expect(result.stages[11].x.lower().isFinite() &&
               result.stages[11].x.lower().value() == AD::Rational(0) &&
               result.stages[11].x.upper().isPlusInfinity(),
           "ordinary widening contract");
    expect(result.stages[12].bottom, "bottom contract");
}

#ifdef SVF_HAVE_ELINA
void compareSequences(const SequenceResult& expected,
                      const SequenceResult& actual)
{
    expect(expected.stages.size() == actual.stages.size(),
           "differential stage count");
    for (std::size_t index = 0; index < expected.stages.size(); ++index)
    {
        const Stage& lhs = expected.stages[index];
        const Stage& rhs = actual.stages[index];
        const std::string prefix = "differential " + lhs.name + ": ";
        expect(lhs.name == rhs.name, prefix + "stage identity");
        expect(lhs.x == rhs.x, prefix + "x bound");
        expect(lhs.y == rhs.y, prefix + "y bound");
        expect(lhs.z == rhs.z, prefix + "z bound");
        expect(lhs.relation == rhs.relation, prefix + "query tri-state");
        expect(lhs.top == rhs.top, prefix + "top predicate");
        expect(lhs.bottom == rhs.bottom, prefix + "bottom predicate");
    }
}

void checkUnsupportedFallbacks(int expectedRounding)
{
    AD::ElinaOctagonDomain point = AD::ElinaOctagonDomain::fromConstraints({
        AD::equal(AD::LinearExpression(X),
                  AD::LinearExpression(AD::Rational(0)))
    });

    AD::ElinaOctagonDomain canonical(point);
    canonical.canonicalize();
    expect(canonical.lastOperation().approximation ==
               AD::ApproximationKind::UnsupportedFallback,
           "canonicalize/minimize fallback was not explicit");

    AD::ElinaOctagonDomain top = AD::ElinaOctagonDomain::top();
    AD::WideningPolicy threshold({AD::Rational(0)});
    AD::ElinaOctagonDomain widened = point.widen(top, threshold);
    expect(widened.lastOperation().approximation ==
               AD::ApproximationKind::UnsupportedFallback,
           "threshold widening fallback was not explicit");

    AD::ElinaOctagonDomain narrowed = top.narrow(point);
    expect(narrowed.lastOperation().approximation ==
               AD::ApproximationKind::UnsupportedFallback,
           "narrowing fallback was not explicit");
    expect(std::fegetround() == expectedRounding,
           "unsupported fallback leaked ELINA's FPU rounding mode");
}
#endif

} // namespace

int main()
{
    expect(AD::octagonBackendAvailable(AD::NumericalBackendKind::Native),
           "native Octagon factory capability changed");
    const int originalRounding = std::fegetround();
    expect(originalRounding != -1, "could not read initial FPU mode");
    expect(std::fesetround(FE_TOWARDZERO) == 0,
           "could not establish the caller FPU mode");
    const int callerRounding = std::fegetround();

    const SequenceResult native =
        runFiniteSequence<AD::OctagonDomain>(callerRounding);
    checkNativeOracle(native);

#ifndef SVF_HAVE_ELINA
    expect(!AD::octagonBackendAvailable(AD::NumericalBackendKind::Elina),
           "disabled ELINA Octagon was reported available");
    std::unique_ptr<AD::NumericalDomain> selected = AD::makeOctagonDomain(
        AD::NumericalBackendKind::Native);
    expect(selected->isDomain<AD::OctagonDomain>(),
           "default factory did not preserve the native B0 backend");
    expect(std::fegetround() == callerRounding,
           "native factory changed the FPU mode");
    expect(std::fesetround(originalRounding) == 0,
           "could not restore initial FPU mode");
    std::cout << "ELINAOctagonAdapterTest: PASS (native-only build)\n";
#else
    expect(AD::octagonBackendAvailable(AD::NumericalBackendKind::Elina),
           "ELINA Octagon factory is unavailable");
    const auto capabilities = AD::ElinaOctagonDomain::capabilities();
    expect(capabilities.exactLinearAssignment &&
               capabilities.exactLinearAssumption &&
               capabilities.parallelAssignment && capabilities.substitution &&
               capabilities.projection && capabilities.expandFold &&
               capabilities.widening && capabilities.topologicalClosure &&
               !capabilities.canonicalization && !capabilities.minimization &&
               !capabilities.narrowing && !capabilities.thresholdWidening,
           "ELINA Octagon capability table is inconsistent");

    const SequenceResult elina =
        runFiniteSequence<AD::ElinaOctagonDomain>(callerRounding);
    compareSequences(native, elina);
    checkUnsupportedFallbacks(callerRounding);

    AD::ElinaOctagonDomain point = AD::ElinaOctagonDomain::fromConstraints({
        AD::equal(AD::LinearExpression(X),
                  AD::LinearExpression(AD::Rational(5)))
    });
    AD::ElinaOctagonDomain top = AD::ElinaOctagonDomain::top();
    expect(point.subsetOf(top) == AD::CheckResult::True,
           "ELINA true inclusion was not proved");
    expect(top.subsetOf(point) == AD::CheckResult::False,
           "ELINA exact false inclusion lost its tri-state result");

    std::unique_ptr<AD::NumericalDomain> selected = AD::makeOctagonDomain(
        AD::NumericalBackendKind::Elina);
    expect(selected->isDomain<AD::ElinaOctagonDomain>(),
           "factory did not select the ELINA Octagon class");
    expect(std::fegetround() == callerRounding,
           "ELINA factory leaked its FPU rounding mode");
    expect(std::fesetround(originalRounding) == 0,
           "could not restore initial FPU mode");
    std::cout << "ELINAOctagonAdapterTest: PASS\n";
#endif
    return EXIT_SUCCESS;
}
