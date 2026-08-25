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
        repetitions, nativeResult);
    const auto nativeExportStart = Clock::now();
    for (ConvexPolyhedraState& result : nativeExports)
        (void)result.toConstraints();
    const double nativeExport =
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  nativeExportStart)
            .count() /
        repetitions;

    std::vector<ApronValue> strictExports(repetitions, strictResult);
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
        repetitions;

    std::vector<ApronValue> lazyExports(repetitions, lazyResult);
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
        repetitions;

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
        repetitions, nativeClipped);
    const auto nativeClipExportStart = Clock::now();
    for (ConvexPolyhedraState& result : nativeClipExports)
        (void)result.toConstraints();
    const double nativeClipExport =
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  nativeClipExportStart)
            .count() /
        repetitions;

    std::vector<ApronValue> strictClipExports(repetitions, strictClipped);
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
        repetitions;

    std::vector<ApronValue> lazyClipExports(repetitions, lazyClipped);
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
        repetitions;

    if (!apronMatches(strictManager, nativeClipped, strictClipped) ||
        !apronMatches(lazyManager, nativeClipped, lazyClipped))
        throw std::runtime_error("clip benchmark implementations disagree");
    std::cout << "incremental_clip," << dimensions << ',' << clips.size()
              << ',' << nativeClipMilliseconds << ','
              << strictClipMilliseconds << ',' << lazyClipMilliseconds << ','
              << nativeClipExport << ',' << strictClipExport << ','
              << lazyClipExport << '\n';
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
        for (std::size_t dimensions = 2; dimensions <= 6; ++dimensions)
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
