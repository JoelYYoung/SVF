#include "AE/Core/Expression.h"
#include "AE/Core/NumericalDomainFactory.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace AD = SVF::AbstractDomain;

namespace
{

using Clock = std::chrono::steady_clock;

const std::vector<std::string> OperationNames = {
    "assign", "assume", "bound",   "copy",    "join",        "meet",
    "widen",  "forget", "project", "closure", "canonicalize"};

struct Options
{
    std::vector<AD::NumericalBackendKind> backends = {
        AD::NumericalBackendKind::Native, AD::NumericalBackendKind::Elina};
    std::vector<AD::DomainKind> domains = {AD::DomainKind::Octagon,
                                           AD::DomainKind::ConvexPolyhedra};
    std::vector<std::size_t> dimensions = {4, 8, 16, 32, 64, 128, 256};
    std::vector<std::string> shapes = {"sparse", "dense", "degenerate"};
    std::size_t iterations = 1;
    bool headerOnly = false;
};

struct Measurement
{
    std::string status = "ok";
    std::uint64_t elapsedNanoseconds = 0;
    std::uint64_t checksum = 0;
    bool hasMetadata = false;
    AD::OperationMetadata metadata;
    std::string error;
};

struct WorkloadDescription
{
    std::size_t inputConstraints = 0;
    std::size_t componentCount = 0;
    std::size_t maxComponent = 0;
    std::size_t coefficientBits = 0;
    std::string detail;
};

std::string sanitize(std::string value)
{
    std::replace(value.begin(), value.end(), '\t', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
}

const char* domainName(AD::DomainKind domain)
{
    switch (domain)
    {
    case AD::DomainKind::Octagon:
        return "octagon";
    case AD::DomainKind::ConvexPolyhedra:
        return "polyhedra";
    default:
        return "unsupported";
    }
}

const char* approximationName(AD::ApproximationKind approximation)
{
    switch (approximation)
    {
    case AD::ApproximationKind::Exact:
        return "exact";
    case AD::ApproximationKind::SoundOverApproximation:
        return "sound-over-approximation";
    case AD::ApproximationKind::UnsupportedFallback:
        return "unsupported-fallback";
    }
    return "unknown";
}

std::unique_ptr<AD::NumericalDomain> cloneNumerical(
    const AD::NumericalDomain& domain)
{
    std::unique_ptr<AD::AbstractDomain> copy = domain.clone();
    return std::unique_ptr<AD::NumericalDomain>(
        static_cast<AD::NumericalDomain*>(copy.release()));
}

std::vector<AD::Variable> makeVariables(std::size_t dimension)
{
    std::vector<AD::Variable> variables;
    variables.reserve(dimension);
    for (std::size_t index = 0; index < dimension; ++index)
        variables.emplace_back(index + 1, AD::NumericType::integer());
    return variables;
}

AD::LinearConstraintSet makeConstraints(
    AD::DomainKind domain, const std::vector<AD::Variable>& variables,
    const std::string& shape, std::int64_t limit)
{
    AD::LinearConstraintSet constraints;
    const AD::LinearExpression zero(AD::Rational(0));
    const AD::LinearExpression upper{AD::Rational(limit)};
    constraints.reserve(variables.size() * 4);
    for (AD::Variable variable : variables)
    {
        constraints.push_back(
            AD::greaterEqual(AD::LinearExpression(variable), zero));
        constraints.push_back(
            AD::lessEqual(AD::LinearExpression(variable), upper));
    }

    if (shape == "sparse")
    {
        for (std::size_t index = 1; index < variables.size(); index += 2)
        {
            const AD::LinearExpression difference =
                AD::LinearExpression(variables[index]) -
                AD::LinearExpression(variables[index - 1]);
            constraints.push_back(AD::lessEqual(
                difference, AD::LinearExpression(AD::Rational(1))));
        }
    }
    else if (shape == "dense")
    {
        if (domain == AD::DomainKind::Octagon)
        {
            for (std::size_t lhs = 0; lhs < variables.size(); ++lhs)
            {
                for (std::size_t rhs = lhs + 1; rhs < variables.size(); ++rhs)
                {
                    constraints.push_back(AD::lessEqual(
                        AD::LinearExpression(variables[lhs]) -
                            AD::LinearExpression(variables[rhs]),
                        AD::LinearExpression(AD::Rational(limit))));
                }
            }
        }
        else
        {
            const std::size_t rows = std::min<std::size_t>(8, variables.size());
            for (std::size_t row = 0; row < rows; ++row)
            {
                AD::LinearExpression expression;
                std::int64_t coefficientSum = 0;
                for (std::size_t index = 0; index < variables.size(); ++index)
                {
                    const std::int64_t coefficient =
                        static_cast<std::int64_t>((index + row) % 3 + 1);
                    expression.setCoefficient(variables[index],
                                              AD::Rational(coefficient));
                    coefficientSum += coefficient;
                }
                constraints.push_back(AD::lessEqual(
                    expression, AD::LinearExpression(AD::Rational(
                                    (limit * coefficientSum) / 2))));
            }
        }
    }
    else if (shape == "degenerate")
    {
        for (std::size_t index = 1; index < variables.size(); ++index)
        {
            const AD::LinearConstraint equality =
                AD::equal(AD::LinearExpression(variables[index]),
                          AD::LinearExpression(variables.front()));
            constraints.push_back(equality);
            constraints.push_back(equality);
        }
    }
    else
        throw std::invalid_argument("unknown workload shape: " + shape);
    return constraints;
}

std::size_t integerBits(const mpz_class& value)
{
    const std::string text = value.get_str(2);
    return text.front() == '-' ? text.size() - 1 : text.size();
}

std::size_t rationalBits(const AD::Rational& value)
{
    return std::max(integerBits(value.value().get_num()),
                    integerBits(value.value().get_den()));
}

WorkloadDescription describeWorkload(AD::DomainKind domain,
                                     const std::vector<AD::Variable>& variables,
                                     const std::string& shape,
                                     const AD::LinearConstraintSet& constraints)
{
    WorkloadDescription description;
    description.inputConstraints = constraints.size();
    for (const AD::LinearConstraint& constraint : constraints)
    {
        description.coefficientBits =
            std::max(description.coefficientBits,
                     rationalBits(constraint.expression().constant()));
        for (const auto& term : constraint.expression().terms())
            description.coefficientBits = std::max(description.coefficientBits,
                                                   rationalBits(term.second));
    }
    if (shape == "sparse")
    {
        description.componentCount = (variables.size() + 1) / 2;
        description.maxComponent = std::min<std::size_t>(2, variables.size());
        description.detail = "disjoint-pair-relations";
    }
    else if (shape == "dense")
    {
        description.componentCount = 1;
        description.maxComponent = variables.size();
        description.detail = domain == AD::DomainKind::Octagon
                                 ? "octagon-all-pairs-differences"
                                 : "polyhedra-dense-affine-rows";
    }
    else
    {
        description.componentCount = 1;
        description.maxComponent = variables.size();
        description.detail = "duplicate-star-equalities";
    }
    return description;
}

std::unique_ptr<AD::NumericalDomain> makeState(
    AD::DomainKind domain, AD::NumericalBackendKind backend,
    const AD::LinearConstraintSet& constraints)
{
    std::unique_ptr<AD::NumericalDomain> state =
        AD::makeNumericalDomain(domain, false, backend);
    state->assumeAll(constraints);
    return state;
}

AD::LinearExpression makeBoundExpression(
    const std::vector<AD::Variable>& variables)
{
    AD::LinearExpression expression;
    for (std::size_t index = 0; index < variables.size(); ++index)
    {
        const std::int64_t coefficient =
            static_cast<std::int64_t>(index % 3 + 1);
        expression.setCoefficient(variables[index], AD::Rational(coefficient));
    }
    return expression;
}

Measurement measureMutation(
    const AD::NumericalDomain& base, std::size_t iterations,
    const std::function<void(AD::NumericalDomain&)>& operation)
{
    Measurement result;
    try
    {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration)
        {
            std::unique_ptr<AD::NumericalDomain> work = cloneNumerical(base);
            const Clock::time_point start = Clock::now();
            operation(*work);
            const Clock::time_point finish = Clock::now();
            result.elapsedNanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finish -
                                                                     start)
                    .count());
            result.metadata = work->lastOperation();
            result.hasMetadata = true;
            result.checksum ^=
                static_cast<std::uint64_t>(work->isBottom()) + iteration + 1;
        }
    }
    catch (const std::exception& error)
    {
        result.status = "error";
        result.error = error.what();
    }
    return result;
}

Measurement measureBound(const AD::NumericalDomain& base,
                         const AD::LinearExpression& expression,
                         std::size_t iterations)
{
    Measurement result;
    try
    {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration)
        {
            const Clock::time_point start = Clock::now();
            const AD::Interval value = base.bound(expression);
            const Clock::time_point finish = Clock::now();
            result.elapsedNanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finish -
                                                                     start)
                    .count());
            result.checksum ^= value.toString().size() + iteration + 1;
        }
    }
    catch (const std::exception& error)
    {
        result.status = "error";
        result.error = error.what();
    }
    return result;
}

Measurement measureCopy(const AD::NumericalDomain& base, std::size_t iterations)
{
    Measurement result;
    try
    {
        for (std::size_t iteration = 0; iteration < iterations; ++iteration)
        {
            const Clock::time_point start = Clock::now();
            std::unique_ptr<AD::NumericalDomain> copy = cloneNumerical(base);
            const Clock::time_point finish = Clock::now();
            result.elapsedNanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finish -
                                                                     start)
                    .count());
            result.checksum ^=
                static_cast<std::uint64_t>(copy->isBottom()) + iteration + 1;
        }
    }
    catch (const std::exception& error)
    {
        result.status = "error";
        result.error = error.what();
    }
    return result;
}

void printHeader()
{
    std::cout
        << "schema_version\tbackend\tdomain\tdimension\tshape\tshape_detail"
           "\tinput_constraints\tinput_h_rows\tcomponent_count\tmax_component"
           "\tcoefficient_bits\tinput_v_rows\toperation"
           "\titerations\tstatus\telapsed_ns\tns_per_iteration\tchecksum"
           "\tapproximation\texact\tbest\treason\toutput_constraints"
           "\toutput_h_rows\toutput_v_rows\tcount_note\tallocation_count"
           "\terror\n";
}

void printMeasurement(AD::NumericalBackendKind backend, AD::DomainKind domain,
                      std::size_t dimension, const std::string& shape,
                      const WorkloadDescription& workload,
                      const std::string& operation, std::size_t iterations,
                      const Measurement& measurement)
{
    std::cout << "2\t" << AD::numericalBackendName(backend) << '\t'
              << domainName(domain) << '\t' << dimension << '\t' << shape
              << '\t' << workload.detail << '\t' << workload.inputConstraints
              << '\t' << workload.inputConstraints << '\t'
              << workload.componentCount << '\t' << workload.maxComponent
              << '\t' << workload.coefficientBits << "\tNA\t" << operation
              << '\t' << iterations << '\t' << measurement.status << '\t';
    if (measurement.status == "ok")
    {
        const double average =
            static_cast<double>(measurement.elapsedNanoseconds) /
            static_cast<double>(iterations);
        std::cout << measurement.elapsedNanoseconds << '\t' << std::fixed
                  << std::setprecision(2) << average << '\t'
                  << measurement.checksum << '\t';
    }
    else
        std::cout << "NA\tNA\tNA\t";

    if (measurement.hasMetadata)
    {
        std::cout << approximationName(measurement.metadata.approximation)
                  << '\t' << (measurement.metadata.exact ? "true" : "false")
                  << '\t' << (measurement.metadata.best ? "true" : "false")
                  << '\t' << sanitize(measurement.metadata.reason);
    }
    else
        std::cout << "NA\tNA\tNA\t";
    std::cout << "\tNA\tNA\tNA"
                 "\tpublic export may materialize or canonicalize the state"
                 "\tNA\t"
              << sanitize(measurement.error) << '\n';
}

void printMeasurement(AD::NumericalBackendKind backend, AD::DomainKind domain,
                      std::size_t dimension, const std::string& shape,
                      const std::string& operation, std::size_t iterations,
                      const Measurement& measurement)
{
    const std::vector<AD::Variable> variables = makeVariables(dimension);
    const AD::LinearConstraintSet constraints =
        makeConstraints(domain, variables, shape, 20);
    printMeasurement(backend, domain, dimension, shape,
                     describeWorkload(domain, variables, shape, constraints),
                     operation, iterations, measurement);
}

void printUnavailable(AD::NumericalBackendKind backend, AD::DomainKind domain,
                      std::size_t dimension, const std::string& shape)
{
    const std::vector<AD::Variable> variables = makeVariables(dimension);
    const AD::LinearConstraintSet constraints =
        makeConstraints(domain, variables, shape, 20);
    const WorkloadDescription workload =
        describeWorkload(domain, variables, shape, constraints);
    for (const std::string& operation : OperationNames)
    {
        Measurement result;
        result.status = "unavailable";
        result.error = "backend not configured in this build";
        printMeasurement(backend, domain, dimension, shape, workload, operation,
                         0, result);
    }
}

void runCase(AD::NumericalBackendKind backend, AD::DomainKind domain,
             std::size_t dimension, const std::string& shape,
             std::size_t iterations)
{
    if (!AD::numericalBackendAvailable(domain, backend))
    {
        printUnavailable(backend, domain, dimension, shape);
        return;
    }

    const std::vector<AD::Variable> variables = makeVariables(dimension);
    try
    {
        const AD::LinearConstraintSet baseConstraints =
            makeConstraints(domain, variables, shape, 10);
        const AD::LinearConstraintSet nextConstraints =
            makeConstraints(domain, variables, shape, 20);
        std::unique_ptr<AD::NumericalDomain> base =
            makeState(domain, backend, baseConstraints);
        std::unique_ptr<AD::NumericalDomain> next =
            makeState(domain, backend, nextConstraints);
        const AD::LinearExpression boundExpression =
            makeBoundExpression(variables);
        const AD::LinearExpression assignment =
            AD::LinearExpression(variables.front()) +
            AD::LinearExpression(AD::Rational(1));
        const AD::LinearConstraint assumption =
            AD::lessEqual(AD::LinearExpression(variables.front()),
                          AD::LinearExpression(AD::Rational(5)));
        std::vector<AD::Variable> retained;
        for (std::size_t index = 0; index < variables.size(); index += 2)
            retained.push_back(variables[index]);

        printMeasurement(
            backend, domain, dimension, shape, "assign", iterations,
            measureMutation(*base, iterations, [&](AD::NumericalDomain& state) {
                state.assign(variables.back(), assignment);
            }));
        printMeasurement(
            backend, domain, dimension, shape, "assume", iterations,
            measureMutation(*base, iterations, [&](AD::NumericalDomain& state) {
                state.assume(assumption);
            }));
        printMeasurement(backend, domain, dimension, shape, "bound", iterations,
                         measureBound(*base, boundExpression, iterations));
        printMeasurement(backend, domain, dimension, shape, "copy", iterations,
                         measureCopy(*base, iterations));
        printMeasurement(
            backend, domain, dimension, shape, "join", iterations,
            measureMutation(*base, iterations, [&](AD::NumericalDomain& state) {
                state.joinWith(*next);
            }));
        printMeasurement(
            backend, domain, dimension, shape, "meet", iterations,
            measureMutation(*base, iterations, [&](AD::NumericalDomain& state) {
                state.meetWith(*next);
            }));
        printMeasurement(
            backend, domain, dimension, shape, "widen", iterations,
            measureMutation(*base, iterations, [&](AD::NumericalDomain& state) {
                state.widenWith(*next);
            }));
        printMeasurement(
            backend, domain, dimension, shape, "forget", iterations,
            measureMutation(*base, iterations, [&](AD::NumericalDomain& state) {
                state.forget(variables[variables.size() / 2]);
            }));
        printMeasurement(
            backend, domain, dimension, shape, "project", iterations,
            measureMutation(*base, iterations, [&](AD::NumericalDomain& state) {
                state.project(retained);
            }));
        printMeasurement(
            backend, domain, dimension, shape, "closure", iterations,
            measureMutation(*base, iterations,
                            [](AD::NumericalDomain& state) { state.close(); }));
        printMeasurement(
            backend, domain, dimension, shape, "canonicalize", iterations,
            measureMutation(*base, iterations, [](AD::NumericalDomain& state) {
                state.canonicalize();
            }));
    }
    catch (const std::exception& error)
    {
        for (const std::string& operation : OperationNames)
        {
            Measurement result;
            result.status = "setup-error";
            result.error = error.what();
            printMeasurement(backend, domain, dimension, shape, operation, 0,
                             result);
        }
    }
}

std::string requireValue(int& index, int argc, char** argv)
{
    if (++index >= argc)
        throw std::invalid_argument(std::string("missing value after ") +
                                    argv[index - 1]);
    return argv[index];
}

Options parseOptions(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--header")
            options.headerOnly = true;
        else if (argument == "--backend")
        {
            const std::string value = requireValue(index, argc, argv);
            if (value == "native")
                options.backends = {AD::NumericalBackendKind::Native};
            else if (value == "elina")
                options.backends = {AD::NumericalBackendKind::Elina};
            else
                throw std::invalid_argument("unknown backend: " + value);
        }
        else if (argument == "--domain")
        {
            const std::string value = requireValue(index, argc, argv);
            if (value == "octagon")
                options.domains = {AD::DomainKind::Octagon};
            else if (value == "polyhedra")
                options.domains = {AD::DomainKind::ConvexPolyhedra};
            else
                throw std::invalid_argument("unknown domain: " + value);
        }
        else if (argument == "--dimension")
        {
            const std::string value = requireValue(index, argc, argv);
            options.dimensions = {static_cast<std::size_t>(std::stoull(value))};
            if (options.dimensions.front() == 0)
                throw std::invalid_argument("dimension must be positive");
        }
        else if (argument == "--shape")
        {
            const std::string value = requireValue(index, argc, argv);
            if (value != "sparse" && value != "dense" && value != "degenerate")
                throw std::invalid_argument("unknown shape: " + value);
            options.shapes = {value};
        }
        else if (argument == "--iterations")
        {
            const std::string value = requireValue(index, argc, argv);
            options.iterations = static_cast<std::size_t>(std::stoull(value));
            if (options.iterations == 0)
                throw std::invalid_argument("iterations must be positive");
        }
        else if (argument == "--help")
        {
            std::cout
                << "usage: NumericalDomainOperationBenchmark [--backend "
                   "native|elina] [--domain octagon|polyhedra] [--dimension "
                   "N] [--shape sparse|dense|degenerate] [--iterations N] "
                   "[--header]\n";
            std::exit(EXIT_SUCCESS);
        }
        else
            throw std::invalid_argument("unknown argument: " + argument);
    }
    return options;
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = parseOptions(argc, argv);
        printHeader();
        if (options.headerOnly)
            return EXIT_SUCCESS;
        for (AD::NumericalBackendKind backend : options.backends)
        {
            for (AD::DomainKind domain : options.domains)
            {
                for (std::size_t dimension : options.dimensions)
                {
                    for (const std::string& shape : options.shapes)
                        runCase(backend, domain, dimension, shape,
                                options.iterations);
                }
            }
        }
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "NumericalDomainOperationBenchmark: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
