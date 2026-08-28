//===- OctagonStorageBenchmark.cpp -- Dense/sparse/component DBM study ---===//

#include "AE/Core/OctagonDomain.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace SVF::AbstractDomain;

namespace
{

volatile std::uint64_t benchmarkSink = 0;

std::size_t opposite(std::size_t node)
{
    return node ^ 1U;
}

std::size_t storedIndex(std::size_t row, std::size_t column)
{
    return column + ((row + 1) * (row + 1)) / 2;
}

std::size_t matrixIndex(std::size_t row, std::size_t column)
{
    return column > row ? storedIndex(opposite(column), opposite(row))
                        : storedIndex(row, column);
}

std::size_t matrixSize(std::size_t variables)
{
    return 2 * variables * (variables + 1);
}

const Bound& infinityBound()
{
    static const Bound value = Bound::plusInfinity();
    return value;
}

const Bound& zeroBound()
{
    static const Bound value = Bound::finite(Rational());
    return value;
}

void setMinimum(Bound& slot, const Bound& value)
{
    if (value < slot)
        slot = value;
}

class DenseHalfMatrix
{
public:
    explicit DenseHalfMatrix(std::size_t variables)
        : variables_(variables), matrix_(matrixSize(variables))
    {
        for (std::size_t node = 0; node < nodes(); ++node)
            matrix_[matrixIndex(node, node)] = zeroBound();
    }

    std::size_t variables() const { return variables_; }
    std::size_t nodes() const { return 2 * variables_; }

    const Bound& at(std::size_t row, std::size_t column) const
    {
        return matrix_[matrixIndex(row, column)];
    }

    void setMin(std::size_t row, std::size_t column, const Bound& value)
    {
        setMinimum(matrix_[matrixIndex(row, column)], value);
    }

    void close()
    {
        for (std::size_t middle = 0; middle < nodes(); ++middle)
        {
            for (std::size_t row = 0; row < nodes(); ++row)
            {
                const Bound left = at(row, middle);
                if (left.isPlusInfinity())
                    continue;
                for (std::size_t column = 0; column < nodes(); ++column)
                {
                    const Bound& right = at(middle, column);
                    if (right.isPlusInfinity())
                        continue;
                    setMin(row, column, Bound::add(left, right));
                }
            }
        }
        strongClose();
    }

    std::size_t allocatedSlots() const { return matrix_.size(); }
    std::size_t componentCount() const { return variables_ == 0 ? 0 : 1; }

    std::size_t finiteSlots() const
    {
        return static_cast<std::size_t>(std::count_if(
            matrix_.begin(), matrix_.end(),
            [](const Bound& bound) { return bound.isFinite(); }));
    }

private:
    void strongClose()
    {
        const Rational two(2);
        for (std::size_t row = 0; row < nodes(); ++row)
        {
            const Bound lhs = at(row, opposite(row));
            if (lhs.isPlusInfinity())
                continue;
            for (std::size_t column = 0; column < nodes(); ++column)
            {
                const Bound& rhs = at(opposite(column), column);
                if (rhs.isPlusInfinity())
                    continue;
                setMin(row, column, Bound::divideByPositive(
                                        Bound::add(lhs, rhs), two));
            }
        }
    }

    std::size_t variables_;
    std::vector<Bound> matrix_;
};

class SparseFiniteMatrix
{
public:
    explicit SparseFiniteMatrix(std::size_t variables)
        : variables_(variables), outgoing_(nodes()), incoming_(nodes())
    {
    }

    std::size_t variables() const { return variables_; }
    std::size_t nodes() const { return 2 * variables_; }

    const Bound& at(std::size_t row, std::size_t column) const
    {
        const auto value = values_.find(matrixIndex(row, column));
        if (value != values_.end())
            return value->second;
        return row == column ? zeroBound() : infinityBound();
    }

    void setMin(std::size_t row, std::size_t column, const Bound& value)
    {
        if (value.isPlusInfinity())
            return;
        const std::size_t key = matrixIndex(row, column);
        const auto found = values_.find(key);
        if (found != values_.end())
        {
            setMinimum(found->second, value);
            return;
        }
        if (row == column && !(value < zeroBound()))
            return;
        values_.emplace(key, value);
        addLogicalEdge(row, column);
        const std::size_t coherentRow = opposite(column);
        const std::size_t coherentColumn = opposite(row);
        if (matrixIndex(coherentRow, coherentColumn) == key)
            addLogicalEdge(coherentRow, coherentColumn);
    }

    void close()
    {
        for (std::size_t middle = 0; middle < nodes(); ++middle)
        {
            const std::vector<std::size_t> rows(incoming_[middle].begin(),
                                                incoming_[middle].end());
            const std::vector<std::size_t> columns(outgoing_[middle].begin(),
                                                   outgoing_[middle].end());
            for (std::size_t row : rows)
            {
                const Bound left = at(row, middle);
                for (std::size_t column : columns)
                    setMin(row, column,
                           Bound::add(left, at(middle, column)));
            }
        }
        strongClose();
    }

    std::size_t allocatedSlots() const { return values_.size(); }
    std::size_t finiteSlots() const { return values_.size() + nodes(); }
    std::size_t componentCount() const { return variables_ == 0 ? 0 : 1; }

private:
    void addLogicalEdge(std::size_t row, std::size_t column)
    {
        if (row == column)
            return;
        outgoing_[row].insert(column);
        incoming_[column].insert(row);
    }

    void strongClose()
    {
        std::vector<std::size_t> upperNodes;
        std::vector<std::size_t> lowerNodes;
        for (std::size_t node = 0; node < nodes(); ++node)
        {
            if (at(node, opposite(node)).isFinite())
                upperNodes.push_back(node);
            if (at(opposite(node), node).isFinite())
                lowerNodes.push_back(node);
        }
        const Rational two(2);
        for (std::size_t row : upperNodes)
        {
            const Bound lhs = at(row, opposite(row));
            for (std::size_t column : lowerNodes)
            {
                const Bound& rhs = at(opposite(column), column);
                setMin(row, column, Bound::divideByPositive(
                                        Bound::add(lhs, rhs), two));
            }
        }
    }

    std::size_t variables_;
    std::unordered_map<std::size_t, Bound> values_;
    std::vector<std::unordered_set<std::size_t>> outgoing_;
    std::vector<std::unordered_set<std::size_t>> incoming_;
};

class ComponentMatrix
{
public:
    explicit ComponentMatrix(std::size_t variables)
        : variables_(variables), owner_(variables, missing()),
          local_(variables, missing())
    {
    }

    ComponentMatrix(const ComponentMatrix& other)
        : variables_(other.variables_), owner_(other.owner_),
          local_(other.local_), components_(other.components_.size())
    {
        for (std::size_t index = 0; index < other.components_.size(); ++index)
        {
            if (other.components_[index])
                components_[index] =
                    std::make_unique<Component>(*other.components_[index]);
        }
    }

    ComponentMatrix& operator=(const ComponentMatrix& other)
    {
        if (this == &other)
            return *this;
        ComponentMatrix copy(other);
        *this = std::move(copy);
        return *this;
    }

    ComponentMatrix(ComponentMatrix&&) noexcept = default;
    ComponentMatrix& operator=(ComponentMatrix&&) noexcept = default;

    std::size_t variables() const { return variables_; }
    std::size_t nodes() const { return 2 * variables_; }

    const Bound& at(std::size_t row, std::size_t column) const
    {
        if (row == column)
            return zeroBound();
        const std::size_t rowVariable = row / 2;
        const std::size_t columnVariable = column / 2;
        if (owner_[rowVariable] == missing() ||
            owner_[rowVariable] != owner_[columnVariable])
            return infinityBound();
        const Component& component = *components_[owner_[rowVariable]];
        return component.matrix.at(2 * local_[rowVariable] + row % 2,
                                   2 * local_[columnVariable] + column % 2);
    }

    void setMin(std::size_t row, std::size_t column, const Bound& value)
    {
        if (value.isPlusInfinity())
            return;
        const std::size_t rowVariable = row / 2;
        const std::size_t columnVariable = column / 2;
        ensure(rowVariable);
        ensure(columnVariable);
        merge(owner_[rowVariable], owner_[columnVariable]);
        Component& component = *components_[owner_[rowVariable]];
        component.matrix.setMin(2 * local_[rowVariable] + row % 2,
                                2 * local_[columnVariable] + column % 2,
                                value);
    }

    void close()
    {
        while (true)
        {
            for (const auto& component : components_)
            {
                if (component)
                    component->matrix.close();
            }

            std::vector<std::size_t> anchoredVariables;
            for (std::size_t variable = 0; variable < variables_; ++variable)
            {
                if (owner_[variable] == missing())
                    continue;
                if (at(2 * variable, 2 * variable + 1).isFinite() ||
                    at(2 * variable + 1, 2 * variable).isFinite())
                    anchoredVariables.push_back(variable);
            }
            if (anchoredVariables.size() < 2)
                return;

            const std::size_t firstOwner = owner_[anchoredVariables.front()];
            bool needsMerge = false;
            for (std::size_t variable : anchoredVariables)
                needsMerge = needsMerge || owner_[variable] != firstOwner;
            if (!needsMerge)
                return;
            for (std::size_t variable : anchoredVariables)
                merge(owner_[anchoredVariables.front()], owner_[variable]);
        }
    }

    std::size_t allocatedSlots() const
    {
        std::size_t result = 0;
        for (const auto& component : components_)
        {
            if (component)
                result += component->matrix.allocatedSlots();
        }
        return result;
    }

    std::size_t finiteSlots() const
    {
        std::size_t result = 0;
        for (const auto& component : components_)
        {
            if (component)
                result += component->matrix.finiteSlots();
        }
        return result;
    }

    std::size_t componentCount() const
    {
        return static_cast<std::size_t>(std::count_if(
            components_.begin(), components_.end(),
            [](const auto& component) { return component != nullptr; }));
    }

private:
    struct Component
    {
        explicit Component(std::vector<std::size_t> values)
            : variables(std::move(values)), matrix(variables.size())
        {
        }

        std::vector<std::size_t> variables;
        DenseHalfMatrix matrix;
    };

    static std::size_t missing()
    {
        return std::numeric_limits<std::size_t>::max();
    }

    void ensure(std::size_t variable)
    {
        if (owner_[variable] != missing())
            return;
        owner_[variable] = components_.size();
        local_[variable] = 0;
        components_.push_back(
            std::make_unique<Component>(std::vector<std::size_t>{variable}));
    }

    void merge(std::size_t lhs, std::size_t rhs)
    {
        if (lhs == rhs)
            return;
        const Component& left = *components_[lhs];
        const Component& right = *components_[rhs];
        std::vector<std::size_t> variables = left.variables;
        variables.insert(variables.end(), right.variables.begin(),
                         right.variables.end());
        std::sort(variables.begin(), variables.end());
        auto merged = std::make_unique<Component>(variables);
        std::map<std::size_t, std::size_t> nextLocal;
        for (std::size_t index = 0; index < variables.size(); ++index)
            nextLocal.emplace(variables[index], index);

        const auto copyComponent = [&](const Component& source)
        {
            for (std::size_t row = 0; row < source.matrix.nodes(); ++row)
            {
                for (std::size_t column = 0;
                     column < source.matrix.nodes(); ++column)
                {
                    const Bound& value = source.matrix.at(row, column);
                    if (!value.isFinite() || row == column)
                        continue;
                    const std::size_t rowVariable =
                        source.variables[row / 2];
                    const std::size_t columnVariable =
                        source.variables[column / 2];
                    merged->matrix.setMin(
                        2 * nextLocal.at(rowVariable) + row % 2,
                        2 * nextLocal.at(columnVariable) + column % 2,
                        value);
                }
            }
        };
        copyComponent(left);
        copyComponent(right);

        const std::size_t mergedIndex = components_.size();
        components_[lhs].reset();
        components_[rhs].reset();
        components_.push_back(std::move(merged));
        for (std::size_t index = 0; index < variables.size(); ++index)
        {
            owner_[variables[index]] = mergedIndex;
            local_[variables[index]] = index;
        }
    }

    std::size_t variables_;
    std::vector<std::size_t> owner_;
    std::vector<std::size_t> local_;
    std::vector<std::unique_ptr<Component>> components_;
};

struct Edge
{
    std::size_t row;
    std::size_t column;
    Bound value;
};

struct Input
{
    std::vector<Edge> edges;
    std::vector<Edge> updates;
};

Input generateInput(std::size_t variables, std::size_t componentSize,
                    bool anchored, std::uint64_t seed)
{
    if (variables == 0 || componentSize == 0)
        throw std::invalid_argument("variables and component size must be positive");
    std::mt19937_64 generator(seed);
    std::uniform_int_distribution<int> potentialDistribution(-20, 20);
    std::uniform_int_distribution<int> slackDistribution(1, 9);
    std::vector<int> potential(2 * variables);
    for (std::size_t variable = 0; variable < variables; ++variable)
    {
        potential[2 * variable] = potentialDistribution(generator);
        potential[2 * variable + 1] = -potential[2 * variable];
    }

    const auto safeBound = [&](std::size_t row, std::size_t column,
                               int slack)
    {
        return Bound::finite(Rational(potential[column] - potential[row] +
                                      slack));
    };

    Input result;
    for (std::size_t begin = 0; begin < variables; begin += componentSize)
    {
        const std::size_t end = std::min(begin + componentSize, variables);
        const std::size_t size = end - begin;
        for (std::size_t offset = 0; offset < size; ++offset)
        {
            const std::size_t variable = begin + offset;
            const std::size_t next = begin + (offset + 1) % size;
            result.edges.push_back(
                {2 * variable, 2 * next,
                 safeBound(2 * variable, 2 * next,
                           slackDistribution(generator))});
            result.edges.push_back(
                {2 * next, 2 * variable,
                 safeBound(2 * next, 2 * variable,
                           slackDistribution(generator))});
            if (size > 2)
            {
                std::uniform_int_distribution<std::size_t> member(0, size - 1);
                const std::size_t randomVariable = begin + member(generator);
                result.edges.push_back(
                    {2 * variable, 2 * randomVariable,
                     safeBound(2 * variable, 2 * randomVariable,
                               slackDistribution(generator))});
            }
            result.updates.push_back(
                {2 * variable, 2 * next,
                 safeBound(2 * variable, 2 * next, 0)});
            if (anchored)
            {
                result.edges.push_back(
                    {2 * variable, 2 * variable + 1,
                     safeBound(2 * variable, 2 * variable + 1,
                               slackDistribution(generator))});
                result.edges.push_back(
                    {2 * variable + 1, 2 * variable,
                     safeBound(2 * variable + 1, 2 * variable,
                               slackDistribution(generator))});
            }
        }
    }
    return result;
}

template <typename MatrixT>
MatrixT build(std::size_t variables, const std::vector<Edge>& edges)
{
    MatrixT result(variables);
    for (const Edge& edge : edges)
        result.setMin(edge.row, edge.column, edge.value);
    return result;
}

std::uint64_t mix(std::uint64_t state, std::uint64_t value)
{
    state ^= value;
    state *= UINT64_C(1099511628211);
    return state;
}

template <typename MatrixT>
std::uint64_t semanticChecksum(const MatrixT& state)
{
    std::uint64_t result = UINT64_C(1469598103934665603);
    for (std::size_t row = 0; row < state.nodes(); ++row)
    {
        for (std::size_t column = 0; column < state.nodes(); ++column)
        {
            const Bound& value = state.at(row, column);
            if (!value.isFinite())
                continue;
            result = mix(result, row);
            result = mix(result, column);
            for (unsigned char byte : value.toString())
                result = mix(result, byte);
        }
    }
    return result;
}

LinearExpression nodeDifference(std::size_t row, std::size_t column)
{
    LinearExpression expression;
    expression.setCoefficient(Variable(static_cast<std::uint32_t>(row / 2 + 1)),
                              Rational(row % 2 == 0 ? 1 : -1));
    const Variable columnVariable(
        static_cast<std::uint32_t>(column / 2 + 1));
    expression.setCoefficient(
        columnVariable,
        expression.coefficient(columnVariable) +
            Rational(column % 2 == 0 ? -1 : 1));
    return expression;
}

void validateAgainstProduction(bool anchored)
{
    constexpr std::size_t variables = 6;
    const Input input = generateInput(variables, 3, anchored, 17);
    DenseHalfMatrix dense = build<DenseHalfMatrix>(variables, input.edges);
    SparseFiniteMatrix sparse =
        build<SparseFiniteMatrix>(variables, input.edges);
    ComponentMatrix components = build<ComponentMatrix>(variables, input.edges);
    dense.close();
    sparse.close();
    components.close();
    const std::uint64_t denseChecksum = semanticChecksum(dense);
    const std::uint64_t sparseChecksum = semanticChecksum(sparse);
    const std::uint64_t componentChecksum = semanticChecksum(components);
    if (denseChecksum != sparseChecksum ||
        denseChecksum != componentChecksum)
    {
        for (std::size_t row = 0; row < dense.nodes(); ++row)
        {
            for (std::size_t column = 0; column < dense.nodes(); ++column)
            {
                if (dense.at(row, column) != sparse.at(row, column))
                    throw std::runtime_error(
                        "sparse closure mismatch at (" +
                        std::to_string(row) + "," +
                        std::to_string(column) + "): dense=" +
                        dense.at(row, column).toString() + " sparse=" +
                        sparse.at(row, column).toString());
                if (dense.at(row, column) != components.at(row, column))
                    throw std::runtime_error(
                        "component closure mismatch at (" +
                        std::to_string(row) + "," +
                        std::to_string(column) + "): dense=" +
                        dense.at(row, column).toString() + " component=" +
                        components.at(row, column).toString());
            }
        }
    }

    std::vector<VariableDeclaration> declarations;
    for (std::size_t variable = 0; variable < variables; ++variable)
        declarations.push_back(
            {Variable(static_cast<std::uint32_t>(variable + 1)),
             NumericType::real(), "v" + std::to_string(variable)});
    const VariableEnvironment environment(std::move(declarations));
    OctagonConfig config;
    config.integerTightening = false;
    OctagonState oracle = OctagonState::top(environment, config);
    for (const Edge& edge : input.edges)
    {
        oracle.assume(lessEqual(nodeDifference(edge.row, edge.column),
                                LinearExpression(edge.value.value())));
    }
    oracle.close();
    for (std::size_t row = 0; row < dense.nodes(); ++row)
    {
        for (std::size_t column = 0; column < dense.nodes(); ++column)
        {
            const Bound expected = dense.at(row, column);
            const Bound actual = oracle.bound(nodeDifference(row, column)).upper();
            if (expected != actual)
                throw std::runtime_error(
                    "benchmark closure disagrees with production Octagon");
        }
    }
}

struct Options
{
    std::string scheme = "dense-half";
    std::string workload = "lookup";
    std::size_t variables = 256;
    std::size_t componentSize = 8;
    bool anchored = false;
    std::size_t operations = 1000;
    std::size_t repetitions = 7;
    std::uint64_t seed = 1;
    bool header = false;
};

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
        if (argument == "--scheme")
            result.scheme = value;
        else if (argument == "--workload")
            result.workload = value;
        else if (argument == "--variables")
            result.variables = std::stoull(value);
        else if (argument == "--component-size")
            result.componentSize = std::stoull(value);
        else if (argument == "--topology")
            result.anchored = value == "anchored";
        else if (argument == "--operations")
            result.operations = std::stoull(value);
        else if (argument == "--repetitions")
            result.repetitions = std::stoull(value);
        else if (argument == "--seed")
            result.seed = std::stoull(value);
        else
            throw std::invalid_argument("unknown option: " + argument);
    }
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

template <typename MatrixT>
void runBenchmark(const Options& options, const Input& input)
{
    MatrixT raw = build<MatrixT>(options.variables, input.edges);
    MatrixT closed = raw;
    closed.close();
    const std::uint64_t checksum = semanticChecksum(closed);
    std::vector<double> samples;
    samples.reserve(options.repetitions);

    for (std::size_t sample = 0; sample <= options.repetitions; ++sample)
    {
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t local = 0;
        if (options.workload == "lookup")
        {
            for (std::size_t operation = 0; operation < options.operations;
                 ++operation)
            {
                const std::size_t row =
                    (operation * UINT64_C(11400714819323198485) + 17) %
                    closed.nodes();
                const std::size_t column =
                    (operation * UINT64_C(7046029254386353131) + 31) %
                    closed.nodes();
                local += closed.at(row, column).isFinite();
            }
        }
        else if (options.workload == "copy-update")
        {
            for (std::size_t operation = 0; operation < options.operations;
                 ++operation)
            {
                MatrixT copy = closed;
                const Edge& edge = input.updates[operation % input.updates.size()];
                copy.setMin(edge.row, edge.column, edge.value);
                local += copy.allocatedSlots();
            }
        }
        else if (options.workload == "close")
        {
            for (std::size_t operation = 0; operation < options.operations;
                 ++operation)
            {
                MatrixT copy = raw;
                copy.close();
                local += copy.finiteSlots();
            }
        }
        else if (options.workload == "incremental-close")
        {
            for (std::size_t operation = 0; operation < options.operations;
                 ++operation)
            {
                MatrixT copy = closed;
                const Edge& edge = input.updates[operation % input.updates.size()];
                copy.setMin(edge.row, edge.column, edge.value);
                copy.close();
                local += copy.finiteSlots();
            }
        }
        else
        {
            throw std::invalid_argument("unknown workload: " +
                                        options.workload);
        }
        const auto stop = std::chrono::steady_clock::now();
        benchmarkSink ^= local;
        if (sample != 0)
        {
            const double nanoseconds =
                std::chrono::duration<double, std::nano>(stop - start).count();
            samples.push_back(nanoseconds /
                              static_cast<double>(options.operations));
        }
    }

    std::cout << options.scheme << ',' << options.workload << ','
              << (options.anchored ? "anchored" : "block") << ','
              << options.variables << ',' << options.componentSize << ','
              << options.operations << ',' << options.repetitions << ','
              << std::fixed << std::setprecision(3)
              << percentile(samples, 0.5) << ',' << percentile(samples, 0.95)
              << ',' << closed.finiteSlots() << ',' << closed.allocatedSlots()
              << ',' << closed.componentCount() << ',' << checksum << ','
              << peakRssBytes() << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options options = parseOptions(argc, argv);
        if (options.header)
        {
            std::cout << "scheme,workload,topology,variables,component_size,"
                         "operations,repetitions,median_ns_per_op,p95_ns_per_op,"
                         "finite_slots,allocated_bound_slots,component_count,"
                         "checksum,peak_rss_bytes\n";
            return EXIT_SUCCESS;
        }
        validateAgainstProduction(false);
        validateAgainstProduction(true);
        const Input input = generateInput(
            options.variables, options.componentSize, options.anchored,
            options.seed);
        if (options.scheme == "dense-half")
            runBenchmark<DenseHalfMatrix>(options, input);
        else if (options.scheme == "sparse-finite")
            runBenchmark<SparseFiniteMatrix>(options, input);
        else if (options.scheme == "component-dense")
            runBenchmark<ComponentMatrix>(options, input);
        else
            throw std::invalid_argument("unknown scheme: " + options.scheme);
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Octagon storage benchmark: FAIL: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
