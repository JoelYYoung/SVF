//===- OctagonApronBenchmark.cpp -- Native/APRON API timing comparison ---===//

#include "ApronPolyhedraTestSupport.h"
#include "AE/Core/OctagonDomain.h"

extern "C"
{
#include "oct.h"
}

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

using namespace SVF::AbstractDomain;
using namespace SVF::test;

namespace
{
using Clock = std::chrono::steady_clock;

struct Options
{
    std::string implementation;
    std::string workload;
    std::string topology;
    std::size_t variables = 0;
    std::size_t componentSize = 0;
    std::size_t operations = 0;
    std::size_t repetitions = 0;
    bool header = false;
};

class Manager
{
public:
    Manager() : manager_(oct_manager_alloc())
    {
        if (manager_ == nullptr)
            throw std::runtime_error("cannot allocate APRON Octagon manager");
    }
    ~Manager() { ap_manager_free(manager_); }
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    ap_manager_t* get() const { return manager_; }

private:
    ap_manager_t* manager_;
};

VariableEnvironment environmentOf(std::size_t dimensions)
{
    std::vector<VariableDeclaration> declarations;
    declarations.reserve(dimensions);
    for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
        declarations.push_back(
            {Variable(static_cast<std::uint32_t>(dimension + 1)),
             NumericType::real(), "v" + std::to_string(dimension)});
    return VariableEnvironment(std::move(declarations));
}

LinearConstraintSet constraintsOf(const VariableEnvironment& environment,
                                  std::size_t componentSize,
                                  bool anchored, std::int64_t phase)
{
    LinearConstraintSet result;
    const std::size_t dimensions = environment.size();
    for (std::size_t begin = 0; begin < dimensions; begin += componentSize)
    {
        const std::size_t end = std::min(begin + componentSize, dimensions);
        for (std::size_t dimension = begin; dimension < end; ++dimension)
        {
            const Variable variable = environment.variableOf(dimension);
            const std::int64_t potential =
                static_cast<std::int64_t>(dimension % 17) + phase;
            if (anchored)
            {
                result.push_back(lessEqual(
                    LinearExpression(variable),
                    LinearExpression(Rational(potential + 20))));
                result.push_back(greaterEqual(
                    LinearExpression(variable),
                    LinearExpression(Rational(potential - 20))));
            }
            if (dimension + 1 < end)
            {
                const Variable next = environment.variableOf(dimension + 1);
                const std::int64_t nextPotential =
                    static_cast<std::int64_t>((dimension + 1) % 17) + phase;
                LinearExpression forward(variable);
                forward.setCoefficient(next, Rational(-1));
                result.push_back(lessEqual(
                    forward,
                    LinearExpression(Rational(potential - nextPotential + 3))));
                LinearExpression reverse(next);
                reverse.setCoefficient(variable, Rational(-1));
                result.push_back(lessEqual(
                    reverse,
                    LinearExpression(Rational(nextPotential - potential + 3))));
            }
        }
    }
    return result;
}

LinearConstraint updateConstraint(const VariableEnvironment& environment,
                                  std::size_t componentSize)
{
    const std::size_t first = environment.size() / 2;
    const std::size_t componentBegin = (first / componentSize) * componentSize;
    const std::size_t second =
        std::min(componentBegin + componentSize - 1,
                 environment.size() - 1);
    LinearExpression expression(environment.variableOf(first));
    expression.setCoefficient(environment.variableOf(second), Rational(-1));
    return lessEqual(expression, LinearExpression(Rational(3)));
}

template <typename Action>
std::pair<double, double> timeOperation(Action&& action,
                                       std::size_t operations,
                                       std::size_t repetitions)
{
    (void)action();
    std::vector<double> samples;
    samples.reserve(repetitions);
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition)
    {
        const auto start = Clock::now();
        std::uint64_t checksum = 0;
        for (std::size_t operation = 0; operation < operations; ++operation)
            checksum ^= action() + operation;
        const auto finish = Clock::now();
        if (checksum == UINT64_MAX)
            std::cerr << "unreachable checksum\n";
        samples.push_back(
            std::chrono::duration<double, std::nano>(finish - start).count() /
            static_cast<double>(operations));
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t p95 =
        std::min(samples.size() - 1,
                 (samples.size() * 95 + 99) / 100 - 1);
    return {samples[samples.size() / 2], samples[p95]};
}

std::uint64_t peakRssBytes()
{
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument(argv[index]);
        if (argument == "--header")
        {
            options.header = true;
            continue;
        }
        if (index + 1 >= argc)
            throw std::invalid_argument("missing value for " + argument);
        const std::string value(argv[++index]);
        if (argument == "--implementation")
            options.implementation = value;
        else if (argument == "--workload")
            options.workload = value;
        else if (argument == "--topology")
            options.topology = value;
        else if (argument == "--variables")
            options.variables = std::stoull(value);
        else if (argument == "--component-size")
            options.componentSize = std::stoull(value);
        else if (argument == "--operations")
            options.operations = std::stoull(value);
        else if (argument == "--repetitions")
            options.repetitions = std::stoull(value);
        else
            throw std::invalid_argument("unknown option: " + argument);
    }
    if (options.header)
        return options;
    if (options.variables == 0 || options.componentSize == 0 ||
        options.componentSize > options.variables || options.operations == 0 ||
        options.repetitions == 0)
        throw std::invalid_argument("invalid zero/range benchmark option");
    if (options.implementation != "native-octagon" &&
        options.implementation != "native-dense-half" &&
        options.implementation != "native-sparse-finite" &&
        options.implementation != "native-component-dense" &&
        options.implementation != "apron-octagon")
        throw std::invalid_argument("unknown implementation");
    if (options.workload != "construct" &&
        options.workload != "copy-update" && options.workload != "join")
        throw std::invalid_argument("unknown workload");
    if (options.topology != "block" && options.topology != "anchored")
        throw std::invalid_argument("unknown topology");
    return options;
}

void benchmark(const Options& options)
{
    const VariableEnvironment environment = environmentOf(options.variables);
    const bool anchored = options.topology == "anchored";
    const LinearConstraintSet leftConstraints =
        constraintsOf(environment, options.componentSize, anchored, 0);
    const LinearConstraintSet rightConstraints =
        constraintsOf(environment, options.componentSize, anchored, 2);
    const LinearConstraint update =
        updateConstraint(environment, options.componentSize);
    OctagonConfig config;
    config.integerTightening = false;
    if (options.implementation == "native-sparse-finite")
        config.storage = OctagonStorageKind::SparseFinite;
    else if (options.implementation == "native-component-dense")
        config.storage = OctagonStorageKind::ComponentDense;
    else
        config.storage = OctagonStorageKind::DenseHalf;
    std::pair<double, double> timing;
    if (options.implementation != "apron-octagon")
    {
        if (options.workload == "construct")
            timing = timeOperation(
                [&] {
                    const OctagonState value = OctagonState::fromConstraints(
                        environment, leftConstraints, config);
                    return static_cast<std::uint64_t>(value.isBottom());
                }, options.operations, options.repetitions);
        else if (options.workload == "copy-update")
        {
            const OctagonState nativeLeft = OctagonState::fromConstraints(
                environment, leftConstraints, config);
            timing = timeOperation(
                [&] {
                    OctagonState value = nativeLeft;
                    value.assume(update);
                    return static_cast<std::uint64_t>(value.isBottom());
                }, options.operations, options.repetitions);
        }
        else
        {
            const OctagonState nativeLeft = OctagonState::fromConstraints(
                environment, leftConstraints, config);
            const OctagonState nativeRight = OctagonState::fromConstraints(
                environment, rightConstraints, config);
            timing = timeOperation(
                [&] {
                    const OctagonState value = nativeLeft.join(nativeRight);
                    return static_cast<std::uint64_t>(value.isBottom());
                }, options.operations, options.repetitions);
        }
    }
    else
    {
        Manager manager;
        if (options.workload == "construct")
            timing = timeOperation(
                [&] {
                    const ApronValue value = apronFromConstraints(
                        manager.get(), environment, leftConstraints);
                    return static_cast<std::uint64_t>(ap_abstract0_is_bottom(
                        manager.get(), value.get()));
                }, options.operations, options.repetitions);
        else if (options.workload == "copy-update")
        {
            const ApronValue apronLeft = apronFromConstraints(
                manager.get(), environment, leftConstraints);
            timing = timeOperation(
                [&] {
                    const ApronValue value = apronMeetConstraints(
                        manager.get(), apronLeft, environment, {update});
                    return static_cast<std::uint64_t>(ap_abstract0_is_bottom(
                        manager.get(), value.get()));
                }, options.operations, options.repetitions);
        }
        else
        {
            const ApronValue apronLeft = apronFromConstraints(
                manager.get(), environment, leftConstraints);
            const ApronValue apronRight = apronFromConstraints(
                manager.get(), environment, rightConstraints);
            timing = timeOperation(
                [&] {
                    const ApronValue value(
                        manager.get(),
                        ap_abstract0_join(manager.get(), false,
                                          apronLeft.get(), apronRight.get()));
                    return static_cast<std::uint64_t>(ap_abstract0_is_bottom(
                        manager.get(), value.get()));
                }, options.operations, options.repetitions);
        }
    }

    std::cout << options.implementation << ',' << options.workload << ','
              << options.topology << ',' << options.variables << ','
              << options.componentSize << ',' << leftConstraints.size() << ','
              << options.operations << ',' << options.repetitions << ','
              << std::fixed << std::setprecision(3) << timing.first << ','
              << timing.second << ',' << peakRssBytes() << '\n';
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = parseOptions(argc, argv);
        if (options.header)
        {
            std::cout << "implementation,workload,topology,variables,"
                         "component_size,constraints,operations,repetitions,"
                         "median_ns_per_op,p95_ns_per_op,peak_rss_bytes\n";
            return 0;
        }
        benchmark(options);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Octagon APRON benchmark: FAIL: " << error.what() << '\n';
        return 1;
    }
}
