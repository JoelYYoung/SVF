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

void benchmark(ap_manager_t* manager, std::size_t dimensions,
               std::size_t pointCount, std::size_t repetitions)
{
    std::vector<VariableDeclaration> declarations;
    for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
        declarations.push_back({Variable(static_cast<std::uint32_t>(dimension + 1)),
                                NumericType::real(),
                                "v" + std::to_string(dimension)});
    const VariableEnvironment environment(std::move(declarations));
    std::vector<ConvexPolyhedraState> nativePoints;
    std::vector<ApronValue> apronPoints;
    for (std::size_t index = 0; index < pointCount; ++index)
    {
        nativePoints.push_back(nativePoint(environment, index));
        apronPoints.push_back(apronFromState(manager, nativePoints.back()));
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

    ApronValue apronResult(manager,
                           ap_abstract0_bottom(manager, 0, dimensions));
    const double apronMilliseconds = medianMilliseconds(
        [&] {
            ApronValue result(manager,
                              ap_abstract0_bottom(manager, 0, dimensions));
            for (const ApronValue& operand : apronPoints)
                result = ApronValue(manager, ap_abstract0_join(
                                                 manager, false, result.get(),
                                                 operand.get()));
            apronResult = std::move(result);
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

    std::vector<ApronValue> apronExports(repetitions, apronResult);
    const auto apronExportStart = Clock::now();
    for (ApronValue& result : apronExports)
    {
        ap_lincons0_array_t array =
            ap_abstract0_to_lincons_array(manager, result.get());
        ap_lincons0_array_clear(&array);
    }
    const double apronExport =
        std::chrono::duration<double, std::milli>(Clock::now() -
                                                  apronExportStart)
            .count() /
        repetitions;

    if (!apronMatches(manager, nativeResult, apronResult))
        throw std::runtime_error("benchmark implementations disagree");

    std::cout << "point_hull," << dimensions << ',' << pointCount << ','
              << std::fixed << std::setprecision(3) << nativeMilliseconds << ','
              << apronMilliseconds << ','
              << (apronMilliseconds == 0.0
                      ? 0.0
                      : nativeMilliseconds / apronMilliseconds)
              << ',' << nativeExport << ',' << apronExport << '\n';
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
        ApronManager manager;
        std::cout << "case,dimensions,points,native_ms,newpolka_ms,"
                     "native_over_newpolka,native_export_ms,newpolka_export_ms\n";
        for (std::size_t dimensions = 2; dimensions <= 6; ++dimensions)
            benchmark(manager.get(), dimensions, points, repetitions);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Polyhedra/APRON benchmark failed: " << error.what()
                  << '\n';
        return 1;
    }
}
