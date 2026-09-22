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

#ifdef SVF_HAVE_ELINA
void expectResult(AD::CheckResult actual, AD::CheckResult expected,
                  const std::string& message)
{
    if (actual == expected)
        return;
    fail(message + ": expected " + AD::toString(expected) + ", got " +
         AD::toString(actual));
}
#endif

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
    AD::ApproximationKind approximation = AD::ApproximationKind::Exact;
    std::string approximationReason;
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
    const AD::OperationMetadata operation = state.lastOperation();
    result.stages.push_back({name, state.bound(X), state.bound(Y),
                             state.bound(Z), state.entails(relation),
                             state.isTop(), state.isBottom(),
                             operation.approximation, operation.reason});
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
    const AD::LinearConstraint xEqualsZPlusOne = AD::equal(
        AD::LinearExpression(X),
        AD::LinearExpression(Z) + AD::LinearExpression(AD::Rational(1)));

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

    // NumericalDomain has no independent rename primitive. This is the
    // call-binding rename contract: simultaneous fresh binding followed by
    // projection of the old name.
    Domain renamed(bounded);
    renamed.assignParallel({
        {Z, AD::LinearExpression(X)},
        {X, AD::LinearExpression(Y)}
    });
    renamed.forget(Y);
    capture(result, "parallel-rename", renamed, xEqualsZPlusOne,
            expectedRounding);

    bounded.assignParallel({
        {X, AD::LinearExpression(Y)},
        {Y, AD::LinearExpression(X)}
    });
    capture(result, "parallel-assign", bounded, xEqualsYPlusOne,
            expectedRounding);

    Domain expanded(bounded);
    expanded.expand(Y, {Z});
    capture(result, "expand", expanded, xEqualsZPlusOne,
            expectedRounding);

    Domain folded(expanded);
    folded.fold(Y, {Z});
    capture(result, "fold", folded, xEqualsYPlusOne, expectedRounding);

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

struct MixedDimensionResult
{
    AD::Interval integerLowBefore;
    AD::Interval integerHighBefore;
    AD::Interval realLowBefore;
    AD::Interval realHighBefore;
    AD::Interval integerLowAfter;
    AD::Interval integerHighAfter;
    AD::Interval realLowAfter;
    AD::Interval realHighAfter;
};

template <typename Domain>
MixedDimensionResult runMixedDimensionSequence(int expectedRounding)
{
    const AD::Variable integerLow(10, AD::NumericType::integer());
    const AD::Variable realLow(20, AD::NumericType::real());
    const AD::Variable integerHigh(30, AD::NumericType::integer());
    const AD::Variable realHigh(40, AD::NumericType::real());

    // Deliberately alternate types and reverse the within-type insertion
    // order. ELINA must still maintain [integers..., reals...] physical
    // coordinates while preserving stable logical Variable identities.
    Domain state = Domain::top();
    state.assign(realHigh, AD::LinearExpression(AD::Rational(40)));
    state.assign(integerHigh, AD::LinearExpression(AD::Rational(30)));
    state.assign(realLow, AD::LinearExpression(AD::Rational(20)));
    state.assign(integerLow, AD::LinearExpression(AD::Rational(10)));
    MixedDimensionResult result{
        state.bound(integerLow), state.bound(integerHigh),
        state.bound(realLow), state.bound(realHigh),
        AD::Interval::top(), AD::Interval::top(), AD::Interval::top(),
        AD::Interval::top()
    };
    state.forget(integerHigh);
    state.forget(realLow);
    result.integerLowAfter = state.bound(integerLow);
    result.integerHighAfter = state.bound(integerHigh);
    result.realLowAfter = state.bound(realLow);
    result.realHighAfter = state.bound(realHigh);
    expect(std::fegetround() == expectedRounding,
           "mixed add/remove dimensions leaked ELINA's FPU rounding mode");
    return result;
}

void checkMixedDimensionOracle(const MixedDimensionResult& result)
{
    expectSingleton(result.integerLowBefore, 10,
                    "mixed dimension integer-low insertion");
    expectSingleton(result.integerHighBefore, 30,
                    "mixed dimension integer-high insertion");
    expectSingleton(result.realLowBefore, 20,
                    "mixed dimension real-low insertion");
    expectSingleton(result.realHighBefore, 40,
                    "mixed dimension real-high insertion");
    expectSingleton(result.integerLowAfter, 10,
                    "mixed dimension retained integer");
    expect(result.integerHighAfter.isTop(),
           "mixed dimension removed integer");
    expect(result.realLowAfter.isTop(), "mixed dimension removed real");
    expectSingleton(result.realHighAfter, 40,
                    "mixed dimension retained real");
}

void checkNativeOracle(const SequenceResult& result)
{
    expect(result.stages.size() == 16, "finite sequence stage count");
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

    const auto& renamed = result.stages[2];
    expect(renamed.x == AD::Interval::closed(AD::Rational(1),
                                              AD::Rational(9)) &&
               renamed.y.isTop() &&
               renamed.z == AD::Interval::closed(AD::Rational(0),
                                                   AD::Rational(8)) &&
               renamed.relation == AD::CheckResult::True,
           "parallel rename and projection semantics");

    const auto& parallel = result.stages[3];
    expect(parallel.x == AD::Interval::closed(AD::Rational(1),
                                               AD::Rational(9)) &&
               parallel.y == AD::Interval::closed(AD::Rational(0),
                                                   AD::Rational(8)) &&
               parallel.relation == AD::CheckResult::True,
           "parallel assignment simultaneous semantics");

    const auto& expanded = result.stages[4];
    expect(expanded.z == AD::Interval::closed(AD::Rational(0),
                                               AD::Rational(8)) &&
               expanded.relation == AD::CheckResult::True,
           "expand contract");
    const auto& folded = result.stages[5];
    expect(folded.z.isTop() && folded.relation == AD::CheckResult::True,
           "fold contract");

    const auto& modified = result.stages[6];
    expect(modified.z == AD::Interval::closed(AD::Rational(2),
                                               AD::Rational(5)) &&
               modified.relation == AD::CheckResult::True,
           "copy modification semantics");
    expect(result.stages[7].z.isTop(), "copy changed the source state");

    const auto& joined = result.stages[8];
    expect(joined.x == AD::Interval::closed(AD::Rational(0),
                                             AD::Rational(2)) &&
               joined.y == AD::Interval::closed(AD::Rational(1),
                                                 AD::Rational(3)) &&
               joined.relation == AD::CheckResult::True,
           "join contract");

    const auto& met = result.stages[9];
    expect(met.x == AD::Interval::closed(AD::Rational(3), AD::Rational(7)) &&
               met.y == AD::Interval::closed(AD::Rational(4),
                                              AD::Rational(8)) &&
               met.relation == AD::CheckResult::True,
           "meet contract");
    expect(result.stages[10].x.isTop() && !result.stages[10].y.isTop(),
           "forget contract");
    expect(result.stages[11].x.isTop() &&
               result.stages[11].y == AD::Interval::closed(
                   AD::Rational(4), AD::Rational(8)),
           "project contract");
    expectSingleton(result.stages[12].y, 4, "substitution preimage");
    expectSingleton(result.stages[13].x, 1, "integer strict rewrite");
    expect(result.stages[13].relation == AD::CheckResult::True,
           "integer strict entailment");
    expect(result.stages[14].x.lower().isFinite() &&
               result.stages[14].x.lower().value() == AD::Rational(0) &&
               result.stages[14].x.upper().isPlusInfinity(),
           "ordinary widening contract");
    expect(result.stages[15].bottom, "bottom contract");
}

#ifdef SVF_HAVE_ELINA
void compareInterval(const AD::Interval& native,
                     const AD::Interval& elina, bool allowOverapproximation,
                     const std::string& message)
{
    const bool valid = allowOverapproximation
                       ? native.isSubsetOf(elina)
                       : native == elina;
    if (!valid)
        fail(message + ": native=" + native.toString() + ", ELINA=" +
             elina.toString());
}

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
        // Expansion is reconstructed from exported constraints and fixed
        // ELINA fold can report an approximation. For those operations the
        // required differential property is containment of the exact native
        // witness bounds, not accidental representation equality.
        const bool allowOverapproximation =
            lhs.name == "expand" || lhs.name == "fold" ||
            lhs.name == "widen";
        if (allowOverapproximation &&
                rhs.approximation != AD::ApproximationKind::Exact)
            expect(!rhs.approximationReason.empty(),
                   prefix + "approximation lacks an explanation");
        compareInterval(lhs.x, rhs.x, allowOverapproximation,
                        prefix + "x bound");
        compareInterval(lhs.y, rhs.y, allowOverapproximation,
                        prefix + "y bound");
        compareInterval(lhs.z, rhs.z, allowOverapproximation,
                        prefix + "z bound");
        // Native Octagon can decide that a constraint is not entailed. ELINA
        // may conservatively return Unknown when its predicate reports false
        // without flag_exact. Unknown is therefore an admissible loss of
        // decisiveness, but a contradictory definite answer is not.
        const bool compatibleQuery =
            lhs.relation == AD::CheckResult::True
                ? rhs.relation == AD::CheckResult::True
                : lhs.relation == AD::CheckResult::False
                      ? rhs.relation != AD::CheckResult::True
                      : true;
        if (!compatibleQuery)
            fail(prefix + "query tri-state: native=" +
                 AD::toString(lhs.relation) + ", ELINA=" +
                 AD::toString(rhs.relation));
        expect(lhs.top == rhs.top, prefix + "top predicate");
        expect(lhs.bottom == rhs.bottom, prefix + "bottom predicate");
    }
}

void compareMixedDimensions(const MixedDimensionResult& expected,
                            const MixedDimensionResult& actual)
{
    compareInterval(expected.integerLowBefore, actual.integerLowBefore,
                    false, "mixed dimensions integer-low before removal");
    compareInterval(expected.integerHighBefore, actual.integerHighBefore,
                    false, "mixed dimensions integer-high before removal");
    compareInterval(expected.realLowBefore, actual.realLowBefore,
                    false, "mixed dimensions real-low before removal");
    compareInterval(expected.realHighBefore, actual.realHighBefore,
                    false, "mixed dimensions real-high before removal");
    compareInterval(expected.integerLowAfter, actual.integerLowAfter,
                    false, "mixed dimensions integer-low after removal");
    compareInterval(expected.integerHighAfter, actual.integerHighAfter,
                    false, "mixed dimensions integer-high after removal");
    compareInterval(expected.realLowAfter, actual.realLowAfter,
                    false, "mixed dimensions real-low after removal");
    compareInterval(expected.realHighAfter, actual.realHighAfter,
                    false, "mixed dimensions real-high after removal");
}

void checkPredicateTriState(int expectedRounding)
{
    const AD::LinearConstraint relation = AD::equal(
        AD::LinearExpression(Y),
        AD::LinearExpression(X) + AD::LinearExpression(AD::Rational(1)));
    AD::ElinaOctagonDomain unconstrained = AD::ElinaOctagonDomain::top();
    // Fixed ELINA f524156d marks a false sat_lincons result on integer
    // dimensions as incomplete. Without flag_exact, false means Unknown.
    expectResult(unconstrained.entails(relation), AD::CheckResult::Unknown,
                 "inexact false entailment must remain conservative");

    AD::ElinaOctagonDomain point = AD::ElinaOctagonDomain::fromConstraints({
        AD::equal(AD::LinearExpression(X),
                  AD::LinearExpression(AD::Rational(0)))
    });
    expectResult(unconstrained.subsetOf(point), AD::CheckResult::False,
                 "exact false inclusion must remain definite");
    expectResult(point.subsetOf(unconstrained), AD::CheckResult::True,
                 "true inclusion must remain definite");
    expect(std::fegetround() == expectedRounding,
           "tri-state predicate checks leaked ELINA's FPU rounding mode");
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
    const MixedDimensionResult nativeMixed =
        runMixedDimensionSequence<AD::OctagonDomain>(callerRounding);
    checkMixedDimensionOracle(nativeMixed);

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
    const MixedDimensionResult elinaMixed =
        runMixedDimensionSequence<AD::ElinaOctagonDomain>(callerRounding);
    compareMixedDimensions(nativeMixed, elinaMixed);
    checkPredicateTriState(callerRounding);
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
