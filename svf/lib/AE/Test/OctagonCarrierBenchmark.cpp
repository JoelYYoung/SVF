//===- OctagonCarrierBenchmark.cpp -- Production carrier comparison ------===//

#include "AE/Core/OctagonDomain.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace SVF;
using namespace SVF::AbstractDomain;

namespace
{

volatile std::uint64_t benchmarkSink = 0;

struct Options
{
    OctagonStorageKind storage = OctagonStorageKind::DenseHalf;
    std::string workload = "transfer-close";
    std::size_t variables = 128;
    std::size_t packSize = 8;
    bool anchored = false;
    std::size_t operations = 20;
    std::size_t repetitions = 7;
    bool header = false;
};

OctagonStorageKind parseStorage(const std::string& value)
{
    if (value == "dense-half")
        return OctagonStorageKind::DenseHalf;
    if (value == "sparse-finite")
        return OctagonStorageKind::SparseFinite;
    if (value == "component-dense")
        return OctagonStorageKind::ComponentDense;
    throw std::invalid_argument("unknown carrier: " + value);
}

Options parseOptions(int argc, char** argv)
{
    Options result;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument(argv[index]);
        if (argument == "--header")
        {
            result.header = true;
            continue;
        }
        if (index + 1 >= argc)
            throw std::invalid_argument("missing value for " + argument);
        const std::string value(argv[++index]);
        if (argument == "--carrier")
            result.storage = parseStorage(value);
        else if (argument == "--workload")
            result.workload = value;
        else if (argument == "--variables")
            result.variables = std::stoull(value);
        else if (argument == "--pack-size")
            result.packSize = std::stoull(value);
        else if (argument == "--topology")
            result.anchored = value == "anchored";
        else if (argument == "--operations")
            result.operations = std::stoull(value);
        else if (argument == "--repetitions")
            result.repetitions = std::stoull(value);
        else
            throw std::invalid_argument("unknown option: " + argument);
    }
    if (result.variables == 0 || result.packSize == 0 ||
        result.operations == 0 || result.repetitions == 0)
        throw std::invalid_argument("sizes and repetition counts must be positive");
    return result;
}

Variable variable(std::size_t index)
{
    return Variable(static_cast<std::uint32_t>(index + 1));
}

VariableEnvironment makeEnvironment(std::size_t variables)
{
    std::vector<VariableDeclaration> declarations;
    declarations.reserve(variables);
    for (std::size_t index = 0; index < variables; ++index)
        declarations.push_back({variable(index), NumericType::real(),
                                "v" + std::to_string(index)});
    return VariableEnvironment(std::move(declarations));
}

LinearConstraint atMost(Variable value, const Rational& bound)
{
    LinearExpression expression(value);
    expression.setConstant(-bound);
    return {std::move(expression), ConstraintKind::LessEqual};
}

LinearConstraint differenceAtMost(Variable lhs, Variable rhs,
                                  const Rational& bound)
{
    LinearExpression expression(lhs);
    expression.setCoefficient(rhs, Rational(-1));
    expression.setConstant(-bound);
    return {std::move(expression), ConstraintKind::LessEqual};
}

LinearConstraintSet makeConstraints(const Options& options)
{
    LinearConstraintSet result;
    for (std::size_t begin = 0; begin < options.variables;
         begin += options.packSize)
    {
        const std::size_t end =
            std::min(options.variables, begin + options.packSize);
        for (std::size_t index = begin + 1; index < end; ++index)
        {
            result.push_back(differenceAtMost(variable(index),
                                              variable(index - 1), Rational(1)));
            result.push_back(differenceAtMost(variable(index - 1),
                                              variable(index), Rational(1)));
        }
        if (options.anchored)
        {
            result.push_back(atMost(variable(begin), Rational(100)));
            LinearExpression lower(variable(begin));
            lower.setConstant(Rational(100));
            result.emplace_back(std::move(lower), ConstraintKind::GreaterEqual);
        }
    }
    return result;
}

OctagonState buildFixture(const VariableEnvironment& environment,
                          const LinearConstraintSet& constraints,
                          OctagonStorageKind storage)
{
    OctagonConfig config;
    config.storage = storage;
    OctagonState result = OctagonState::top(environment, config);
    for (const LinearConstraint& constraint : constraints)
        result.assume(constraint);
    result.close();
    return result;
}

double percentile(std::vector<double> values, double fraction)
{
    std::sort(values.begin(), values.end());
    const std::size_t index = static_cast<std::size_t>(
        std::ceil(fraction * static_cast<double>(values.size()))) - 1;
    return values[std::min(index, values.size() - 1)];
}

std::uint64_t peakRssBytes()
{
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        throw std::runtime_error("getrusage failed");
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

void run(const Options& options)
{
    const VariableEnvironment environment = makeEnvironment(options.variables);
    const LinearConstraintSet constraints = makeConstraints(options);
    const OctagonState fixture =
        buildFixture(environment, constraints, options.storage);
    const OctagonState oracle =
        buildFixture(environment, constraints, OctagonStorageKind::DenseHalf);
    if (fixture.isEquivalentTo(oracle) != CheckResult::True)
        throw std::runtime_error("selected carrier disagrees with dense oracle");

    OctagonState joinPeer = OctagonState::top(environment);
    for (std::size_t index = 0; index < options.variables; ++index)
        if (index % options.packSize == 0)
            joinPeer.assume(atMost(variable(index), Rational(200)));

    std::vector<double> samples;
    for (std::size_t sample = 0; sample <= options.repetitions; ++sample)
    {
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t local = 0;
        for (std::size_t operation = 0; operation < options.operations;
             ++operation)
        {
            if (options.workload == "transfer-close")
            {
                OctagonState state = OctagonState::top(environment, fixture.config());
                for (const LinearConstraint& constraint : constraints)
                    state.assume(constraint);
                local += state.storageStats().allocatedBoundSlots;
            }
            else if (options.workload == "copy-query")
            {
                OctagonState state = fixture;
                local += state.bound(variable(operation % options.variables))
                             .upper().isFinite();
            }
            else if (options.workload == "join")
            {
                OctagonState state = fixture.join(joinPeer);
                local += state.storageStats().finiteStoredSlots;
            }
            else if (options.workload == "forget")
            {
                OctagonState state = fixture;
                state.forget(variable(operation % options.variables));
                local += state.storageStats().allocatedBoundSlots;
            }
            else
                throw std::invalid_argument("unknown workload: " +
                                            options.workload);
        }
        const auto stop = std::chrono::steady_clock::now();
        benchmarkSink ^= local;
        if (sample != 0)
            samples.push_back(
                std::chrono::duration<double, std::nano>(stop - start).count() /
                static_cast<double>(options.operations));
    }

    const OctagonStorageStats stats = fixture.storageStats();
    std::cout << octagonStorageKindName(options.storage) << ','
              << options.workload << ','
              << (options.anchored ? "anchored" : "block") << ','
              << options.variables << ',' << options.packSize << ','
              << options.operations << ',' << options.repetitions << ','
              << std::fixed << std::setprecision(3)
              << percentile(samples, 0.5) << ',' << percentile(samples, 0.95)
              << ',' << stats.finiteStoredSlots << ','
              << stats.allocatedBoundSlots << ',' << stats.components << ','
              << stats.maximumComponent << ',' << peakRssBytes() << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = parseOptions(argc, argv);
        if (options.header)
        {
            std::cout << "carrier,workload,topology,variables,pack_size,"
                         "operations,repetitions,median_ns_per_op,p95_ns_per_op,"
                         "finite_slots,allocated_bound_slots,components,"
                         "maximum_component,peak_rss_bytes\n";
            return EXIT_SUCCESS;
        }
        run(options);
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Octagon carrier benchmark: FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
