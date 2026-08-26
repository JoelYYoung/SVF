//===- PolyhedraApronBenchmark.cpp -- Native/NewPolka timing comparison --===//

#include "ApronPolyhedraTestSupport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace SVF::AbstractDomain;
using namespace SVF::test;

namespace
{

using Clock = std::chrono::steady_clock;

struct TimedResult
{
    double milliseconds;
    ConvexPolyhedraState native;
};

ConvexPolyhedraState nativePoint(const VariableEnvironment& environment,
                                 std::size_t pointIndex)
{
    LinearConstraintSet constraints;
    for (std::size_t dimension = 0; dimension < environment.size(); ++dimension)
    {
        const std::int64_t value = static_cast<std::int64_t>(
            ((pointIndex + 3) * (dimension + 5) + pointIndex * pointIndex) % 23) -
            11;
        constraints.push_back(equal(
            LinearExpression(environment.variableOf(dimension)),
            LinearExpression(Rational(value))));
    }
    return ConvexPolyhedraState::fromConstraints(environment, constraints);
}

template <typename Action>
double medianMilliseconds(Action&& action, std::size_t repetitions)
{
    std::vector<double> samples;
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition)
    {
        const auto start = Clock::now();
        action();
        const auto finish = Clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(finish - start).count());
    }
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void benchmark(ap_manager_t* strictManager, ap_manager_t* lazyManager,
               std::size_t dimensions,
               std::size_t pointCount, std::size_t repetitions)
{
    // Cold materialization needs independent copies, but allocating one copy
    // per arbitrarily large timing repetition obscures the code under test and
    // can consume gigabytes during profiling runs.
    const std::size_t materializeRepetitions =
        std::min<std::size_t>(repetitions, 4096);
    const double materializeDivisor =
        static_cast<double>(materializeRepetitions);
    std::vector<VariableDeclaration> declarations;
    for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
        declarations.push_back({Variable(static_cast<std::uint32_t>(dimension + 1)),
                                NumericType::real(),
                                "v" + std::to_string(dimension)});
    const VariableEnvironment environment(std::move(declarations));
    std::vector<ConvexPolyhedraState> nativePoints;
    std::vector<ApronValue> strictPoints;
    std::vector<ApronValue> lazyPoints;
    for (std::size_t index = 0; index < pointCount; ++index)
    {
        nativePoints.push_back(nativePoint(environment, index));
        strictPoints.push_back(
            apronFromState(strictManager, nativePoints.back()));
        lazyPoints.push_back(apronFromState(lazyManager, nativePoints.back()));
    }

    ConvexPolyhedraState nativeResult =
        ConvexPolyhedraState::bottom(environment);
    const double nativeMilliseconds = medianMilliseconds(
        [&] {
            ConvexPolyhedraState result =
                ConvexPolyhedraState::bottom(environment);
            for (const ConvexPolyhedraState& operand : nativePoints)
                result = result.join(operand);
            nativeResult = std::move(result);
        },
        repetitions);

    ApronValue strictResult(
        strictManager, ap_abstract0_bottom(strictManager, 0, dimensions));
    const double strictMilliseconds = medianMilliseconds(
        [&] {
            ApronValue result(
                strictManager,
                ap_abstract0_bottom(strictManager, 0, dimensions));
            for (const ApronValue& operand : strictPoints)
                result = ApronValue(
                    strictManager,
                    ap_abstract0_join(strictManager, false, result.get(),
                                      operand.get()));
            strictResult = std::move(result);
        },
        repetitions);

    ApronValue lazyResult(
        lazyManager, ap_abstract0_bottom(lazyManager, 0, dimensions));
    const double lazyMilliseconds = medianMilliseconds(
        [&] {
            ApronValue result(
                lazyManager,
                ap_abstract0_bottom(lazyManager, 0, dimensions));
            for (const ApronValue& operand : lazyPoints)
                result = ApronValue(
                    lazyManager,
                    ap_abstract0_join(lazyManager, false, result.get(),
                                      operand.get()));
            lazyResult = std::move(result);
        },
        repetitions);

    // Force lazy H materialization on fresh copies. Re-exporting the same
    // object would only measure its already warm cache.
    std::vector<ConvexPolyhedraState> nativeExports(
        materializeRepetitions, nativeResult);
    const auto nativeExportStart = Clock::now();
    for (ConvexPolyhedraState& result : nativeExports)
        (void)result.toConstraints();
    const double nativeExport =
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  nativeExportStart)
            .count() /
        materializeDivisor;

    std::vector<ApronValue> strictExports(materializeRepetitions,
                                          strictResult);
    const auto strictExportStart = Clock::now();
    for (ApronValue& result : strictExports)
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(strictManager, result.get());
        ap_lincons0_array_clear(&array);
    }
    const double strictExport =
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  strictExportStart)
            .count() /
        materializeDivisor;

    std::vector<ApronValue> lazyExports(materializeRepetitions, lazyResult);
    const auto lazyExportStart = Clock::now();
    for (ApronValue& result : lazyExports)
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(lazyManager, result.get());
        ap_lincons0_array_clear(&array);
    }
    const double lazyExport =
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  lazyExportStart)
            .count() /
        materializeDivisor;

    if (!apronMatches(strictManager, nativeResult, strictResult) ||
        !apronMatches(lazyManager, nativeResult, lazyResult))
        throw std::runtime_error("benchmark implementations disagree");

    std::cout << "point_hull," << dimensions << ',' << pointCount << ','
              << std::fixed << std::setprecision(3) << nativeMilliseconds << ','
              << strictMilliseconds << ',' << lazyMilliseconds << ','
              << nativeExport << ',' << strictExport << ',' << lazyExport
              << '\n';

    // Materialize the same hull before measuring incremental clipping. This
    // isolates the persistent saturation/polar update from the lazy join
    // policy measured above.
    ConvexPolyhedraState nativeClipBase = nativeResult;
    (void)nativeClipBase.toConstraints();
    ApronValue strictClipBase = strictResult;
    ApronValue lazyClipBase = lazyResult;
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(strictManager, strictClipBase.get());
        ap_lincons0_array_clear(&array);
    }
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(lazyManager, lazyClipBase.get());
        ap_lincons0_array_clear(&array);
    }

    LinearConstraintSet clips;
    clips.reserve(dimensions + 1);
    LinearExpression sum;
    for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
    {
        const Variable variable = environment.variableOf(dimension);
        clips.push_back(lessEqual(LinearExpression(variable),
                                  LinearExpression(Rational(8))));
        sum.setCoefficient(variable, Rational(1));
    }
    clips.push_back(greaterEqual(
        sum, LinearExpression(Rational(-4 *
                                        static_cast<std::int64_t>(dimensions)))));

    ConvexPolyhedraState nativeClipped = nativeClipBase;
    const double nativeClipMilliseconds = medianMilliseconds(
        [&] {
            ConvexPolyhedraState result = nativeClipBase;
            result.assumeAll(clips);
            nativeClipped = std::move(result);
        },
        repetitions);
    ApronValue strictClipped = strictClipBase;
    const double strictClipMilliseconds = medianMilliseconds(
        [&] {
            strictClipped = apronMeetConstraints(
                strictManager, strictClipBase, environment, clips);
        },
        repetitions);
    ApronValue lazyClipped = lazyClipBase;
    const double lazyClipMilliseconds = medianMilliseconds(
        [&] {
            lazyClipped = apronMeetConstraints(
                lazyManager, lazyClipBase, environment, clips);
        },
        repetitions);

    std::vector<ConvexPolyhedraState> nativeClipExports(
        materializeRepetitions, nativeClipped);
    const auto nativeClipExportStart = Clock::now();
    for (ConvexPolyhedraState& result : nativeClipExports)
        (void)result.toConstraints();
    const double nativeClipExport =
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  nativeClipExportStart)
            .count() /
        materializeDivisor;

    std::vector<ApronValue> strictClipExports(materializeRepetitions,
                                              strictClipped);
    const auto strictClipExportStart = Clock::now();
    for (ApronValue& result : strictClipExports)
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(strictManager, result.get());
        ap_lincons0_array_clear(&array);
    }
    const double strictClipExport =
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  strictClipExportStart)
            .count() /
        materializeDivisor;

    std::vector<ApronValue> lazyClipExports(materializeRepetitions,
                                            lazyClipped);
    const auto lazyClipExportStart = Clock::now();
    for (ApronValue& result : lazyClipExports)
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(lazyManager, result.get());
        ap_lincons0_array_clear(&array);
    }
    const double lazyClipExport =
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  lazyClipExportStart)
            .count() /
        materializeDivisor;

    if (!apronMatches(strictManager, nativeClipped, strictClipped) ||
        !apronMatches(lazyManager, nativeClipped, lazyClipped))
        throw std::runtime_error("clip benchmark implementations disagree");
    std::cout << "incremental_clip," << dimensions << ',' << clips.size()
              << ',' << nativeClipMilliseconds << ','
              << strictClipMilliseconds << ',' << lazyClipMilliseconds << ','
              << nativeClipExport << ',' << strictClipExport << ','
              << lazyClipExport << '\n';

    const Variable expandedSource = environment.variableOf(0);
    const Variable firstCopy(
        static_cast<std::uint32_t>(dimensions + 1));
    const Variable secondCopy(
        static_cast<std::uint32_t>(dimensions + 2));
    const std::vector<VariableDeclaration> expandedCopies{
        {firstCopy, NumericType::real(), "expanded0"},
        {secondCopy, NumericType::real(), "expanded1"}};
    ConvexPolyhedraState nativeFolded = nativeClipBase;
    const double nativeExpandFoldMilliseconds = medianMilliseconds(
        [&] {
            ConvexPolyhedraState result = nativeClipBase;
            result.expand(expandedSource, expandedCopies);
            result.fold(expandedSource, {firstCopy, secondCopy});
            nativeFolded = std::move(result);
        },
        repetitions);
    ap_dim_t foldedDimensions[] = {
        SVF::test::apronDimension(environment, expandedSource),
        static_cast<ap_dim_t>(dimensions),
        static_cast<ap_dim_t>(dimensions + 1)};
    ApronValue strictFolded = strictClipBase;
    const double strictExpandFoldMilliseconds = medianMilliseconds(
        [&] {
            ApronValue expanded(
                strictManager,
                ap_abstract0_expand(
                    strictManager, false, strictClipBase.get(),
                    SVF::test::apronDimension(environment, expandedSource),
                    2));
            strictFolded = ApronValue(
                strictManager,
                ap_abstract0_fold(strictManager, false, expanded.get(),
                                  foldedDimensions, 3));
        },
        repetitions);
    ApronValue lazyFolded = lazyClipBase;
    const double lazyExpandFoldMilliseconds = medianMilliseconds(
        [&] {
            ApronValue expanded(
                lazyManager,
                ap_abstract0_expand(
                    lazyManager, false, lazyClipBase.get(),
                    SVF::test::apronDimension(environment, expandedSource),
                    2));
            lazyFolded = ApronValue(
                lazyManager,
                ap_abstract0_fold(lazyManager, false, expanded.get(),
                                  foldedDimensions, 3));
        },
        repetitions);
    if (!apronMatches(strictManager, nativeFolded, strictFolded) ||
        !apronMatches(lazyManager, nativeFolded, lazyFolded))
        throw std::runtime_error(
            "expand/fold benchmark implementations disagree");
    std::cout << "expand_fold," << dimensions << ",2,"
              << nativeExpandFoldMilliseconds << ','
              << strictExpandFoldMilliseconds << ','
              << lazyExpandFoldMilliseconds << ",0,0,0\n";

    std::size_t exportedGeneratorCount = 0;
    const double nativeGeneratorExportMilliseconds = medianMilliseconds(
        [&] {
            ConvexPolyhedraState result = nativeClipBase;
            exportedGeneratorCount = result.toGenerators().size();
        },
        repetitions);
    const double strictGeneratorExportMilliseconds = medianMilliseconds(
        [&] {
            ApronValue result = strictClipBase;
            ap_generator0_array_t generators =
                ap_abstract0_to_generator_array(strictManager, result.get());
            exportedGeneratorCount = generators.size;
            ap_generator0_array_clear(&generators);
        },
        repetitions);
    const double lazyGeneratorExportMilliseconds = medianMilliseconds(
        [&] {
            ApronValue result = lazyClipBase;
            ap_generator0_array_t generators =
                ap_abstract0_to_generator_array(lazyManager, result.get());
            exportedGeneratorCount = generators.size;
            ap_generator0_array_clear(&generators);
        },
        repetitions);
    if (exportedGeneratorCount == 0 && !nativeClipBase.isBottom())
        throw std::runtime_error("generator export benchmark returned empty");
    std::cout << "generator_export," << dimensions << ",1,"
              << nativeGeneratorExportMilliseconds << ','
              << strictGeneratorExportMilliseconds << ','
              << lazyGeneratorExportMilliseconds << ",0,0,0\n";

    // Exercise the non-pointed phase separately. The initial equality leaves
    // a shared line in v0/v1, while every later unconstrained dimension adds
    // another explicit line. The batch then removes those lineality
    // directions before entering the ordinary pointed DD phase.
    LinearExpression difference(environment.variableOf(0));
    difference.setCoefficient(environment.variableOf(1), Rational(-1));
    LinearConstraintSet linealityInitial{
        equal(difference, LinearExpression(Rational(2)))};
    if (dimensions > 2)
    {
        linealityInitial.push_back(greaterEqual(
            LinearExpression(environment.variableOf(2)),
            LinearExpression(Rational(-3))));
    }
    ConvexPolyhedraState nativeLinealityBase =
        ConvexPolyhedraState::fromConstraints(environment, linealityInitial);
    nativeLinealityBase = nativeLinealityBase.join(nativeLinealityBase);
    (void)nativeLinealityBase.toConstraints();
    ApronValue strictLinealityBase =
        apronFromState(strictManager, nativeLinealityBase);
    ApronValue lazyLinealityBase =
        apronFromState(lazyManager, nativeLinealityBase);

    LinearConstraintSet linealityClips{
        lessEqual(LinearExpression(environment.variableOf(0)),
                  LinearExpression(Rational(5))),
        greaterEqual(LinearExpression(environment.variableOf(1)),
                     LinearExpression(Rational(-4)))};
    for (std::size_t dimension = 2; dimension < dimensions; ++dimension)
    {
        linealityClips.push_back(lessEqual(
            LinearExpression(environment.variableOf(dimension)),
            LinearExpression(Rational(
                static_cast<std::int64_t>(dimension + 3)))));
    }

    ConvexPolyhedraState nativeLineality = nativeLinealityBase;
    const double nativeLinealityMilliseconds = medianMilliseconds(
        [&] {
            ConvexPolyhedraState result = nativeLinealityBase;
            result.assumeAll(linealityClips);
            nativeLineality = std::move(result);
        },
        repetitions);
    ApronValue strictLineality = strictLinealityBase;
    const double strictLinealityMilliseconds = medianMilliseconds(
        [&] {
            strictLineality = apronMeetConstraints(
                strictManager, strictLinealityBase, environment,
                linealityClips);
        },
        repetitions);
    ApronValue lazyLineality = lazyLinealityBase;
    const double lazyLinealityMilliseconds = medianMilliseconds(
        [&] {
            lazyLineality = apronMeetConstraints(
                lazyManager, lazyLinealityBase, environment, linealityClips);
        },
        repetitions);

    std::vector<ConvexPolyhedraState> nativeLinealityExports(
        materializeRepetitions, nativeLineality);
    const auto nativeLinealityExportStart = Clock::now();
    for (ConvexPolyhedraState& result : nativeLinealityExports)
        (void)result.toConstraints();
    const double nativeLinealityExport =
        std::chrono::duration<double, std::milli>(
            Clock::now() - nativeLinealityExportStart)
            .count() /
        materializeDivisor;

    std::vector<ApronValue> strictLinealityExports(materializeRepetitions,
                                                   strictLineality);
    const auto strictLinealityExportStart = Clock::now();
    for (ApronValue& result : strictLinealityExports)
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(strictManager, result.get());
        ap_lincons0_array_clear(&array);
    }
    const double strictLinealityExport =
        std::chrono::duration<double, std::milli>(
            Clock::now() - strictLinealityExportStart)
            .count() /
        materializeDivisor;

    std::vector<ApronValue> lazyLinealityExports(materializeRepetitions,
                                                 lazyLineality);
    const auto lazyLinealityExportStart = Clock::now();
    for (ApronValue& result : lazyLinealityExports)
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(lazyManager, result.get());
        ap_lincons0_array_clear(&array);
    }
    const double lazyLinealityExport =
        std::chrono::duration<double, std::milli>(
            Clock::now() - lazyLinealityExportStart)
            .count() /
        materializeDivisor;

    if (!apronMatches(strictManager, nativeLineality, strictLineality) ||
        !apronMatches(lazyManager, nativeLineality, lazyLineality))
        throw std::runtime_error(
            "lineality benchmark implementations disagree");
    std::cout << "lineality_clip," << dimensions << ','
              << linealityClips.size() << ','
              << nativeLinealityMilliseconds << ','
              << strictLinealityMilliseconds << ','
              << lazyLinealityMilliseconds << ','
              << nativeLinealityExport << ',' << strictLinealityExport << ','
              << lazyLinealityExport << '\n';

    // NNC epsilon-representation path: the self-join leaves a lazy V-only
    // value containing both included and closure points, and the strict clip
    // is processed incrementally without closing its boundary.
    LinearConstraintSet nncInitial;
    LinearExpression nncSum;
    for (const VariableDeclaration& declaration : environment.variables())
    {
        nncInitial.push_back(greaterThan(
            LinearExpression(declaration.variable),
            LinearExpression(Rational(-4))));
        nncInitial.push_back(lessEqual(
            LinearExpression(declaration.variable),
            LinearExpression(Rational(5))));
        nncSum.setCoefficient(declaration.variable, Rational(1));
    }
    nncInitial.push_back(lessThan(
        nncSum, LinearExpression(Rational(
                    static_cast<std::int64_t>(dimensions + 1)))));
    ConvexPolyhedraState nativeNNCBase =
        ConvexPolyhedraState::fromConstraints(environment, nncInitial);
    nativeNNCBase = nativeNNCBase.join(nativeNNCBase);
    (void)nativeNNCBase.toConstraints();
    ApronValue strictNNCBase =
        apronFromConstraints(strictManager, environment, nncInitial);
    strictNNCBase = ApronValue(
        strictManager,
        ap_abstract0_join(strictManager, false, strictNNCBase.get(),
                          strictNNCBase.get()));
    ApronValue lazyNNCBase =
        apronFromConstraints(lazyManager, environment, nncInitial);
    lazyNNCBase = ApronValue(
        lazyManager,
        ap_abstract0_join(lazyManager, false, lazyNNCBase.get(),
                          lazyNNCBase.get()));
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(strictManager, strictNNCBase.get());
        ap_lincons0_array_clear(&array);
    }
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(lazyManager, lazyNNCBase.get());
        ap_lincons0_array_clear(&array);
    }

    LinearExpression nncDifference(environment.variableOf(0));
    nncDifference.setCoefficient(environment.variableOf(1), Rational(-1));
    const LinearConstraintSet nncClips{greaterThan(
        nncDifference, LinearExpression(Rational(-3)))};

    ConvexPolyhedraState nativeNNC = nativeNNCBase;
    const double nativeNNCMilliseconds = medianMilliseconds(
        [&] {
            ConvexPolyhedraState result = nativeNNCBase;
            result.assumeAll(nncClips);
            nativeNNC = std::move(result);
        },
        repetitions);
    ApronValue strictNNC = strictNNCBase;
    const double strictNNCMilliseconds = medianMilliseconds(
        [&] {
            strictNNC = apronMeetConstraints(
                strictManager, strictNNCBase, environment, nncClips);
        },
        repetitions);
    ApronValue lazyNNC = lazyNNCBase;
    const double lazyNNCMilliseconds = medianMilliseconds(
        [&] {
            lazyNNC = apronMeetConstraints(
                lazyManager, lazyNNCBase, environment, nncClips);
        },
        repetitions);

    std::vector<ConvexPolyhedraState> nativeNNCExports(
        materializeRepetitions, nativeNNC);
    const auto nativeNNCExportStart = Clock::now();
    for (ConvexPolyhedraState& result : nativeNNCExports)
        (void)result.toConstraints();
    const double nativeNNCExport =
        std::chrono::duration<double, std::milli>(
            Clock::now() - nativeNNCExportStart)
            .count() /
        materializeDivisor;

    const auto apronExport = [&](ap_manager_t* manager,
                                 const ApronValue& source)
    {
        std::vector<ApronValue> exports(materializeRepetitions, source);
        const auto start = Clock::now();
        for (ApronValue& value : exports)
        {
            ap_lincons0_array_t array =
                ap_abstract0_to_lincons_array(manager, value.get());
            ap_lincons0_array_clear(&array);
        }
        return std::chrono::duration<double, std::milli>(Clock::now() - start)
                   .count() /
               materializeDivisor;
    };
    const double strictNNCExport = apronExport(strictManager, strictNNC);
    const double lazyNNCExport = apronExport(lazyManager, lazyNNC);
    if (!apronMatches(strictManager, nativeNNC, strictNNC) ||
        !apronMatches(lazyManager, nativeNNC, lazyNNC))
        throw std::runtime_error("NNC benchmark implementations disagree");
    std::cout << "nnc_clip," << dimensions << ',' << nncClips.size() << ','
              << nativeNNCMilliseconds << ',' << strictNNCMilliseconds << ','
              << lazyNNCMilliseconds << ',' << nativeNNCExport << ','
              << strictNNCExport << ',' << lazyNNCExport << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const std::size_t points =
            argc > 1 ? static_cast<std::size_t>(std::stoul(argv[1])) : 10;
        const std::size_t repetitions =
            argc > 2 ? static_cast<std::size_t>(std::stoul(argv[2])) : 3;
        const std::size_t maxDimensions =
            argc > 3 ? static_cast<std::size_t>(std::stoul(argv[3])) : 6;
        if (maxDimensions < 2)
            throw std::invalid_argument(
                "maximum benchmark dimension must be at least two");
        ApronManager strictManager;
        ApronManager lazyManager;
        ap_funopt_t lazyJoin =
            ap_manager_get_funopt(lazyManager.get(), AP_FUNID_JOIN);
        lazyJoin.algorithm = -1;
        ap_manager_set_funopt(lazyManager.get(), AP_FUNID_JOIN, &lazyJoin);
        std::cout << "case,dimensions,operands,native_operation_ms,"
                     "newpolka_strict_operation_ms,"
                     "newpolka_lazy_operation_ms,native_materialize_ms,"
                     "newpolka_strict_materialize_ms,"
                     "newpolka_lazy_materialize_ms\n";
        for (std::size_t dimensions = 2; dimensions <= maxDimensions;
             ++dimensions)
            benchmark(strictManager.get(), lazyManager.get(), dimensions,
                      points, repetitions);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Polyhedra/APRON benchmark failed: " << error.what()
                  << '\n';
        return 1;
    }
}
