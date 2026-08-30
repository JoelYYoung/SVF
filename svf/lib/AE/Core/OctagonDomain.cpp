//===- OctagonDomain.cpp -- Exact GMP octagon relational backend --------===//

#include "AE/Core/OctagonDomain.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SVF::AbstractDomain
{

namespace
{

std::size_t positiveNode(Dimension dimension)
{
    return 2 * dimension;
}

std::size_t negativeNode(Dimension dimension)
{
    return 2 * dimension + 1;
}

std::size_t opposite(std::size_t node)
{
    return node ^ 1U;
}

class OctagonStorage final
{
public:
    struct ScratchMatrixTag
    {
    };

    explicit OctagonStorage(const VariableEnvironment& environment,
                            OctagonStorageKind kind, bool bottom = false)
        : OctagonStorage(extractVariableKinds(environment), kind, bottom)
    {
    }

    explicit OctagonStorage(std::vector<NumericKind> variableKinds,
                            OctagonStorageKind kind, bool bottom = false)
        : dimensions(variableKinds.size()),
          variableKinds(std::move(variableKinds)),
          kind_(kind),
          bottom(bottom)
    {
        initializeCarrier();
        const std::size_t count = nodes();
        for (std::size_t node = 0; node < count; ++node)
            set(node, node, zero());
    }

    OctagonStorage(std::vector<NumericKind> variableKinds,
                   OctagonStorageKind kind, ScratchMatrixTag)
        : dimensions(variableKinds.size()),
          variableKinds(std::move(variableKinds)),
          kind_(kind)
    {
        initializeCarrier();
    }

    std::unique_ptr<OctagonStorage> clone() const
    {
        return std::make_unique<OctagonStorage>(*this);
    }

    std::size_t nodes() const
    {
        return 2 * dimensions;
    }

    const Bound& at(std::size_t row, std::size_t column) const
    {
        switch (kind_)
        {
        case OctagonStorageKind::DenseHalf:
            return dense_[matrixIndex(row, column)];
        case OctagonStorageKind::SparseFinite: {
            const auto found = sparse_.find(matrixIndex(row, column));
            return found == sparse_.end() ? implicit(row, column)
                                          : found->second;
        }
        case OctagonStorageKind::ComponentDense:
            return componentAt(row, column);
        }
        throw std::logic_error("unknown Octagon storage kind");
    }

    void set(std::size_t row, std::size_t column, const Bound& value)
    {
        switch (kind_)
        {
        case OctagonStorageKind::DenseHalf:
            dense_[matrixIndex(row, column)] = value;
            return;
        case OctagonStorageKind::SparseFinite:
            setSparse(row, column, value);
            return;
        case OctagonStorageKind::ComponentDense:
            setComponent(row, column, value);
            return;
        }
        throw std::logic_error("unknown Octagon storage kind");
    }

    OctagonStorage converted(OctagonStorageKind kind) const
    {
        if (kind == kind_)
            return *this;
        OctagonStorage result(variableKinds, kind, ScratchMatrixTag{});
        for (std::size_t row = 0; row < nodes(); ++row)
            for (std::size_t column = 0; column < nodes(); ++column)
                result.set(row, column, at(row, column));
        result.bottom = bottom;
        result.stronglyClosed = stronglyClosed;
        return result;
    }

    OctagonStorageKind kind() const { return kind_; }

    std::size_t allocatedSlots() const
    {
        switch (kind_)
        {
        case OctagonStorageKind::DenseHalf:
            return dense_.size();
        case OctagonStorageKind::SparseFinite:
            return sparse_.size();
        case OctagonStorageKind::ComponentDense: {
            std::size_t result = 0;
            for (const Component& component : components_)
                result += component.matrix.size();
            return result;
        }
        }
        return 0;
    }

    std::size_t finiteStoredSlots() const
    {
        std::size_t result = 0;
        switch (kind_)
        {
        case OctagonStorageKind::DenseHalf:
            for (const Bound& value : dense_)
                result += static_cast<std::size_t>(value.isFinite());
            break;
        case OctagonStorageKind::SparseFinite:
            for (const auto& entry : sparse_)
                result += static_cast<std::size_t>(entry.second.isFinite());
            // Sparse diagonal zeroes are implicit.
            result += nodes();
            break;
        case OctagonStorageKind::ComponentDense:
            for (const Component& component : components_)
                for (const Bound& value : component.matrix)
                    result += static_cast<std::size_t>(value.isFinite());
            // Diagonal zeroes outside components are implicit.
            for (std::size_t owner : componentOwner_)
                if (owner == noComponent())
                    result += 2;
            break;
        }
        return result;
    }

    /// Rebuilds component membership from the currently finite relation graph.
    /// It is semantically a no-op and recovers physical sparsity after forget.
    void compactComponents()
    {
        if (kind_ != OctagonStorageKind::ComponentDense)
            return;
        std::vector<std::tuple<std::size_t, std::size_t, Bound>> entries;
        for (std::size_t row = 0; row < nodes(); ++row)
            for (std::size_t column = 0; column < nodes(); ++column)
            {
                const Bound& value = at(row, column);
                if (!value.isPlusInfinity() &&
                    !(row == column && value == zero()))
                    entries.emplace_back(row, column, value);
            }
        components_.clear();
        componentOwner_.assign(dimensions, noComponent());
        componentLocal_.assign(dimensions, 0);
        for (const auto& [row, column, value] : entries)
            setComponent(row, column, value);
    }

    std::size_t dimensions;
    std::vector<NumericKind> variableKinds;
    bool bottom = false;
    bool stronglyClosed = true;

private:
    struct Component
    {
        std::vector<std::size_t> variables;
        std::vector<Bound> matrix;
    };

    /// APRON's hmat layout stores triangular 2x2 variable blocks. Coherent
    /// off-diagonal entries share an index. Each diagonal block is fully
    /// stored, so its two main-diagonal entries remain a distinct coherent
    /// pair that normalize() keeps equal. at(row,column) preserves the logical
    /// 2n x 2n matrix interface for all algorithms.
    static std::size_t matrixSize(std::size_t dimensions)
    {
        return 2 * dimensions * (dimensions + 1);
    }

    static std::size_t storedIndex(std::size_t row, std::size_t column)
    {
        return column + ((row + 1) * (row + 1)) / 2;
    }

    static std::size_t matrixIndex(std::size_t row, std::size_t column)
    {
        return column > row ? storedIndex(opposite(column), opposite(row))
                            : storedIndex(row, column);
    }

    static const Bound& zero()
    {
        static const Bound value = Bound::finite(Rational());
        return value;
    }

    static const Bound& infinity()
    {
        static const Bound value = Bound::plusInfinity();
        return value;
    }

    static const Bound& implicit(std::size_t row, std::size_t column)
    {
        return row == column ? zero() : infinity();
    }

    static std::size_t noComponent()
    {
        return std::numeric_limits<std::size_t>::max();
    }

    void initializeCarrier()
    {
        if (kind_ == OctagonStorageKind::DenseHalf)
            dense_.resize(matrixSize(dimensions));
        else if (kind_ == OctagonStorageKind::ComponentDense)
        {
            componentOwner_.assign(dimensions, noComponent());
            componentLocal_.assign(dimensions, 0);
        }
    }

    void setSparse(std::size_t row, std::size_t column, const Bound& value)
    {
        const std::size_t index = matrixIndex(row, column);
        if (value.isPlusInfinity() || (row == column && value == zero()))
            sparse_.erase(index);
        else
            sparse_[index] = value;
    }

    const Bound& componentAt(std::size_t row, std::size_t column) const
    {
        const std::size_t rowVariable = row / 2;
        const std::size_t columnVariable = column / 2;
        const std::size_t owner = componentOwner_[rowVariable];
        if (owner == noComponent() ||
            owner != componentOwner_[columnVariable])
            return implicit(row, column);
        const std::size_t localRow = 2 * componentLocal_[rowVariable] + row % 2;
        const std::size_t localColumn =
            2 * componentLocal_[columnVariable] + column % 2;
        return components_[owner].matrix[matrixIndex(localRow, localColumn)];
    }

    std::size_t createComponent(std::size_t variable)
    {
        const std::size_t owner = components_.size();
        Component component;
        component.variables.push_back(variable);
        component.matrix.resize(matrixSize(1));
        component.matrix[matrixIndex(0, 0)] = zero();
        component.matrix[matrixIndex(1, 1)] = zero();
        components_.push_back(std::move(component));
        componentOwner_[variable] = owner;
        componentLocal_[variable] = 0;
        return owner;
    }

    std::size_t ensureComponent(std::size_t variable)
    {
        if (componentOwner_[variable] == noComponent())
            return createComponent(variable);
        return componentOwner_[variable];
    }

    std::size_t mergeComponents(std::size_t lhsOwner, std::size_t rhsOwner)
    {
        if (lhsOwner == rhsOwner)
            return lhsOwner;
        if (components_[lhsOwner].variables.size() <
            components_[rhsOwner].variables.size())
            std::swap(lhsOwner, rhsOwner);

        Component& lhs = components_[lhsOwner];
        const Component& rhs = components_[rhsOwner];
        const std::size_t oldSize = lhs.variables.size();
        std::vector<Bound> merged(matrixSize(oldSize + rhs.variables.size()));
        for (std::size_t node = 0; node < 2 * (oldSize + rhs.variables.size());
             ++node)
            merged[matrixIndex(node, node)] = zero();
        for (std::size_t row = 0; row < 2 * oldSize; ++row)
            for (std::size_t column = 0; column < 2 * oldSize; ++column)
                merged[matrixIndex(row, column)] =
                    lhs.matrix[matrixIndex(row, column)];
        for (std::size_t row = 0; row < 2 * rhs.variables.size(); ++row)
            for (std::size_t column = 0; column < 2 * rhs.variables.size();
                    ++column)
                merged[matrixIndex(2 * oldSize + row, 2 * oldSize + column)] =
                    rhs.matrix[matrixIndex(row, column)];

        for (std::size_t variable : rhs.variables)
        {
            componentOwner_[variable] = lhsOwner;
            componentLocal_[variable] = lhs.variables.size();
            lhs.variables.push_back(variable);
        }
        lhs.matrix = std::move(merged);

        components_.erase(components_.begin() + rhsOwner);
        for (std::size_t variable = 0; variable < dimensions; ++variable)
            if (componentOwner_[variable] != noComponent() &&
                componentOwner_[variable] > rhsOwner)
                --componentOwner_[variable];
        return lhsOwner > rhsOwner ? lhsOwner - 1 : lhsOwner;
    }

    void setComponent(std::size_t row, std::size_t column, const Bound& value)
    {
        const std::size_t rowVariable = row / 2;
        const std::size_t columnVariable = column / 2;
        if (value.isPlusInfinity())
        {
            const std::size_t owner = componentOwner_[rowVariable];
            if (owner == noComponent() ||
                owner != componentOwner_[columnVariable])
                return;
            const std::size_t localRow =
                2 * componentLocal_[rowVariable] + row % 2;
            const std::size_t localColumn =
                2 * componentLocal_[columnVariable] + column % 2;
            components_[owner].matrix[matrixIndex(localRow, localColumn)] = value;
            return;
        }
        if (row == column && value == zero() &&
            componentOwner_[rowVariable] == noComponent())
            return;
        std::size_t rowOwner = ensureComponent(rowVariable);
        std::size_t columnOwner = ensureComponent(columnVariable);
        const std::size_t owner = mergeComponents(rowOwner, columnOwner);
        const std::size_t localRow =
            2 * componentLocal_[rowVariable] + row % 2;
        const std::size_t localColumn =
            2 * componentLocal_[columnVariable] + column % 2;
        components_[owner].matrix[matrixIndex(localRow, localColumn)] = value;
    }

    static std::vector<NumericKind> extractVariableKinds(
        const VariableEnvironment& environment)
    {
        std::vector<NumericKind> result;
        result.reserve(environment.size());
        for (const VariableDeclaration& declaration : environment.variables())
            result.push_back(declaration.type.kind);
        return result;
    }

    OctagonStorageKind kind_ = OctagonStorageKind::DenseHalf;
    std::vector<Bound> dense_;
    std::unordered_map<std::size_t, Bound> sparse_;
    std::vector<Component> components_;
    std::vector<std::size_t> componentOwner_;
    std::vector<std::size_t> componentLocal_;
};

struct OctagonShape
{
    std::size_t dimensions = 0;
    std::size_t finiteStored = 0;
    std::size_t allocatedSlots = 0;
    std::size_t components = 0;
    std::size_t maximumComponent = 0;
    std::size_t unaryAnchors = 0;
    bool bottom = false;
};

OctagonShape measureShape(const OctagonStorage& state)
{
    OctagonShape result;
    result.dimensions = state.dimensions;
    result.allocatedSlots = state.allocatedSlots();
    result.bottom = state.bottom;
    result.finiteStored = state.finiteStoredSlots();

    std::vector<std::size_t> parent(state.dimensions);
    std::vector<std::size_t> componentSize(state.dimensions, 1);
    for (std::size_t dimension = 0; dimension < state.dimensions; ++dimension)
        parent[dimension] = dimension;
    const auto rootOf = [&](std::size_t start)
    {
        std::size_t root = start;
        while (parent[root] != root)
            root = parent[root];
        return root;
    };
    const auto unite = [&](std::size_t lhs, std::size_t rhs)
    {
        lhs = rootOf(lhs);
        rhs = rootOf(rhs);
        if (lhs == rhs)
            return;
        if (componentSize[lhs] < componentSize[rhs])
            std::swap(lhs, rhs);
        parent[rhs] = lhs;
        componentSize[lhs] += componentSize[rhs];
    };

    for (std::size_t lhs = 0; lhs < state.dimensions; ++lhs)
    {
        const std::size_t positive = positiveNode(lhs);
        const std::size_t negative = negativeNode(lhs);
        if (state.at(positive, negative).isFinite() ||
            state.at(negative, positive).isFinite())
            ++result.unaryAnchors;
        for (std::size_t rhs = lhs + 1; rhs < state.dimensions; ++rhs)
        {
            bool related = false;
            for (std::size_t lhsSign = 0; lhsSign < 2 && !related; ++lhsSign)
                for (std::size_t rhsSign = 0; rhsSign < 2; ++rhsSign)
                    if (state.at(2 * lhs + lhsSign,
                                 2 * rhs + rhsSign).isFinite())
                    {
                        related = true;
                        break;
                    }
            if (related)
                unite(lhs, rhs);
        }
    }
    for (std::size_t dimension = 0; dimension < state.dimensions; ++dimension)
    {
        if (rootOf(dimension) != dimension)
            continue;
        ++result.components;
        result.maximumComponent =
            std::max(result.maximumComponent, componentSize[dimension]);
    }
    return result;
}

std::string densityBucket(const OctagonShape& shape)
{
    if (shape.bottom)
        return "bottom";
    if (shape.finiteStored <= 2 * shape.dimensions)
        return "top";
    if (shape.allocatedSlots == 0)
        return "empty";
    const long double density =
        static_cast<long double>(shape.finiteStored) /
        static_cast<long double>(shape.allocatedSlots);
    if (density <= 0.01L)
        return "le-1pct";
    if (density <= 0.10L)
        return "le-10pct";
    if (density <= 0.50L)
        return "le-50pct";
    if (density < 1.0L)
        return "le-99pct";
    return "dense";
}

struct TelemetryKey
{
    std::string layer;
    std::string operation;
    std::size_t dimensions = 0;
    std::string density;

    bool operator<(const TelemetryKey& other) const
    {
        return std::tie(layer, operation, dimensions, density) <
               std::tie(other.layer, other.operation, other.dimensions,
                        other.density);
    }
};

struct TelemetryValue
{
    std::uint64_t count = 0;
    std::uint64_t totalNanoseconds = 0;
    std::uint64_t maximumNanoseconds = 0;
    std::uint64_t finiteStoredSum = 0;
    std::uint64_t allocatedSlotsSum = 0;
    std::uint64_t componentCountSum = 0;
    std::uint64_t maximumComponentSum = 0;
    std::uint64_t unaryAnchorSum = 0;
    std::uint64_t bottomCount = 0;
};

struct TelemetryState;
TelemetryState& telemetryState();
void flushTelemetry();

struct TelemetryState
{
    TelemetryState()
    {
        const char* configured = std::getenv("SVF_OCTAGON_TELEMETRY");
        if (configured == nullptr || configured[0] == '\0' ||
            std::string(configured) == "0")
            return;
        enabled = true;
        output = configured;
        std::atexit(flushTelemetry);
    }

    bool enabled = false;
    std::string output;
    std::mutex mutex;
    std::map<TelemetryKey, TelemetryValue> values;
};

TelemetryState& telemetryState()
{
    static TelemetryState* state = new TelemetryState();
    return *state;
}

bool telemetryEnabled()
{
    return telemetryState().enabled;
}

void recordTelemetry(const char* layer, const char* operation,
                     std::uint64_t nanoseconds,
                     const OctagonStorage& state, bool sampleShape)
{
    TelemetryState& telemetry = telemetryState();
    if (!telemetry.enabled)
        return;
    OctagonShape shape;
    shape.dimensions = state.dimensions;
    shape.allocatedSlots = state.allocatedSlots();
    std::string density = "not-sampled";
    if (sampleShape)
    {
        shape = measureShape(state);
        density = densityBucket(shape);
    }
    std::lock_guard<std::mutex> lock(telemetry.mutex);
    TelemetryValue& value =
        telemetry.values[{layer, operation, shape.dimensions, density}];
    ++value.count;
    value.totalNanoseconds += nanoseconds;
    value.maximumNanoseconds =
        std::max(value.maximumNanoseconds, nanoseconds);
    value.finiteStoredSum += shape.finiteStored;
    value.allocatedSlotsSum += shape.allocatedSlots;
    value.componentCountSum += shape.components;
    value.maximumComponentSum += shape.maximumComponent;
    value.unaryAnchorSum += shape.unaryAnchors;
    value.bottomCount += static_cast<std::uint64_t>(shape.bottom);
}

void flushTelemetry()
{
    TelemetryState& telemetry = telemetryState();
    std::lock_guard<std::mutex> lock(telemetry.mutex);
    std::ofstream file;
    std::ostream* output = &std::cerr;
    if (telemetry.output != "1" && telemetry.output != "-" &&
        telemetry.output != "stderr")
    {
        file.open(telemetry.output, std::ios::out | std::ios::trunc);
        if (file)
            output = &file;
    }
    *output << "layer,operation,dimensions,density_bucket,count,total_ns,"
               "max_ns,finite_stored_sum,allocated_slots_sum,"
               "component_count_sum,max_component_size_sum,"
               "unary_anchor_sum,bottom_count\n";
    for (const auto& [key, value] : telemetry.values)
    {
        *output << key.layer << ',' << key.operation << ',' << key.dimensions
                << ',' << key.density << ',' << value.count << ','
                << value.totalNanoseconds << ',' << value.maximumNanoseconds
                << ',' << value.finiteStoredSum << ','
                << value.allocatedSlotsSum << ',' << value.componentCountSum
                << ',' << value.maximumComponentSum << ','
                << value.unaryAnchorSum << ',' << value.bottomCount << '\n';
    }
}

class StorageTelemetryScope
{
public:
    StorageTelemetryScope(const char* layer, const char* operation,
                          const OctagonStorage& state,
                          bool sampleShape = false)
        : layer_(layer), operation_(operation), state_(state),
          sampleShape_(sampleShape), enabled_(telemetryEnabled()),
          exceptions_(std::uncaught_exceptions())
    {
        if (enabled_)
            start_ = Clock::now();
    }

    ~StorageTelemetryScope()
    {
        if (!enabled_ || std::uncaught_exceptions() != exceptions_)
            return;
        const std::uint64_t nanoseconds =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Clock::now() - start_)
                    .count());
        recordTelemetry(layer_, operation_, nanoseconds, state_, sampleShape_);
    }

private:
    using Clock = std::chrono::steady_clock;
    const char* layer_;
    const char* operation_;
    const OctagonStorage& state_;
    bool sampleShape_;
    bool enabled_;
    int exceptions_;
    Clock::time_point start_{};
};

OctagonStorage constructStorageForTelemetry(
    const VariableEnvironment& environment, OctagonStorageKind kind, bool bottom)
{
    if (!telemetryEnabled())
        return OctagonStorage(environment, kind, bottom);
    const auto start = std::chrono::steady_clock::now();
    OctagonStorage result(environment, kind, bottom);
    const std::uint64_t nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    recordTelemetry("lifecycle", bottom ? "construct-bottom" : "construct-top",
                    nanoseconds, result, false);
    return result;
}

OctagonStorage copyStorageForTelemetry(const OctagonStorage& source)
{
    if (!telemetryEnabled())
        return source;
    const auto start = std::chrono::steady_clock::now();
    OctagonStorage result(source);
    const std::uint64_t nanoseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    recordTelemetry("lifecycle", "copy", nanoseconds, result, false);
    return result;
}

const OctagonStorage& asOctagon(const OctagonStorage& state)
{
    return static_cast<const OctagonStorage&>(state);
}

OctagonStorage& asOctagon(OctagonStorage& state)
{
    return static_cast<OctagonStorage&>(state);
}

bool isNegativeDiagonal(const Bound& diagonal)
{
    if (diagonal.isMinusInfinity())
        return true;
    if (!diagonal.isFinite())
        return false;
    return diagonal.value().sign() < 0 ||
           (diagonal.value().isZero() && diagonal.isStrict());
}

Rational absolute(const Rational& value)
{
    return value.sign() < 0 ? -value : value;
}

/// Greatest even integer satisfying z <= bound (or z < bound for a strict
/// bound).  Unary integer octagon entries constrain 2*x and must be even.
Bound tightenIntegerUnary(const Bound& bound)
{
    if (!bound.isFinite())
        return bound;
    Rational greatest = bound.isStrict() ? bound.value().ceil() - Rational(1)
                                         : bound.value().floor();
    Rational even = (greatest / Rational(2)).floor() * Rational(2);
    return Bound::finite(std::move(even));
}

} // namespace

const char* octagonStorageKindName(OctagonStorageKind kind)
{
    switch (kind)
    {
    case OctagonStorageKind::DenseHalf:
        return "dense-half";
    case OctagonStorageKind::SparseFinite:
        return "sparse-finite";
    case OctagonStorageKind::ComponentDense:
        return "component-dense";
    }
    return "unknown";
}

OctagonStorageKind octagonStorageKindFromName(const std::string& name)
{
    if (name == "dense-half")
        return OctagonStorageKind::DenseHalf;
    if (name == "sparse-finite")
        return OctagonStorageKind::SparseFinite;
    if (name == "component-dense")
        return OctagonStorageKind::ComponentDense;
    throw std::invalid_argument(
        "unknown Octagon storage carrier '" + name +
        "' (expected dense-half, sparse-finite, or component-dense)");
}

class OctagonState::Impl final
{
public:
    Impl(const VariableEnvironment& environment, OctagonConfig options, bool bottom)
        : options_(std::move(options)),
          state_(constructStorageForTelemetry(environment, options_.storage,
                                               bottom))
    {
    }

    Impl(OctagonConfig options, OctagonStorage state)
        : options_(std::move(options)), state_(std::move(state))
    {
    }

    Impl(const Impl& other)
        : options_(other.options_), state_(copyStorageForTelemetry(other.state_))
    {
    }

    const char* name() const
    {
        return "gmp-octagon";
    }

    DomainCapabilities capabilities() const
    {
        DomainCapabilities result;
        result.strictInequalities = true;
        result.integerTightening = options_.integerTightening;
        result.thresholdWidening = true;
        result.narrowing = true;
        result.parallelAssignments = true;
        result.expressionBounds = true;
        result.backwardAssignments = true;
        result.topologicalClosure = true;
        result.canonicalization = true;
        result.expandFold = true;
        result.operationMetadata = true;
        result.ieeeTreeExpressions = true;
        result.nonlinearTreeExpressions = true;
        return result;
    }

    ApproximationKind assign(OctagonStorage& genericState,
                             const VariableEnvironment& environment, Variable target,
                             const LinearExpression& expression) const
    {
        OctagonStorage& state = asOctagon(genericState);
        requireVariables(environment, expression);
        if (!environment.contains(target))
            throw std::invalid_argument(
                "assignment target is not in relational environment");
        if (environment.typeOf(target).kind == NumericKind::IEEEFloat)
        {
            forget(state, environment.dimensionOf(target));
            return ApproximationKind::UnsupportedFallback;
        }
        if (state.bottom)
            return ApproximationKind::Exact;

        const Dimension targetDimension = environment.dimensionOf(target);
        if (expression.terms().empty())
        {
            forget(state, targetDimension);
            LinearExpression equality(target);
            equality -= expression;
            addConstraint(
                state, environment,
                LinearConstraint(std::move(equality), ConstraintKind::Equal));
            normalize(state, environment);
            return ApproximationKind::Exact;
        }

        if (expression.terms().size() == 1)
        {
            const auto& [source, coefficient] = *expression.terms().begin();
            if ((coefficient == Rational(1) || coefficient == Rational(-1)) &&
                environment.typeOf(source).kind != NumericKind::IEEEFloat)
            {
                const Dimension sourceDimension =
                    environment.dimensionOf(source);
                if (sourceDimension == targetDimension)
                {
                    selfAssign(state, environment, targetDimension,
                               coefficient.sign(), expression.constant());
                }
                else
                {
                    forget(state, targetDimension);
                    LinearExpression equality(target);
                    equality -= expression;
                    addConstraint(state, environment,
                                  LinearConstraint(std::move(equality),
                                                   ConstraintKind::Equal));
                    normalize(state, environment);
                }
                return ApproximationKind::Exact;
            }
        }

        // Strong-update soundness requires killing every old relation of the
        // target first. What follows is the recovery, which used to be absent:
        // the target was simply forgotten.
        //
        // The right-hand side has to be measured before the update, because
        // the update destroys what it is measured against. That matters even
        // when the recovery is the equality below: if the expression reads the
        // target, as in `x := 2*x + y`, then asserting `target = expression`
        // after the strong update is not a weaker fact, it is a false one --
        // the `target` in the expression is the old value and is already gone.
        // The best interval-only image is not enough for an octagon: every
        // objective E +/- y and -E +/- y yields a representable relation
        // between the new target x'=E and an unchanged variable y.  Measure
        // all of them on the incoming state before the strong update.  This
        // is the general octagonal affine-image construction; it does not
        // depend on the number or values of the coefficients in E.
        normalize(state, environment);
        const Interval assigned =
            evaluateInterval(state, environment, expression);
        struct RelationalImage
        {
            Variable variable;
            int sign;
            Bound targetUpper;
            Bound targetLower;
        };
        std::vector<RelationalImage> relationalImages;
        relationalImages.reserve(2 * (environment.size() - 1));
        for (const VariableDeclaration& declaration : environment.variables())
        {
            if (declaration.variable == target)
                continue;
            for (const int sign : {-1, 1})
            {
                LinearExpression positiveObjective = expression;
                positiveObjective.setCoefficient(
                    declaration.variable,
                    positiveObjective.coefficient(declaration.variable) +
                        Rational(sign));
                LinearExpression negativeObjective = -expression;
                negativeObjective.setCoefficient(
                    declaration.variable,
                    negativeObjective.coefficient(declaration.variable) +
                        Rational(sign));
                relationalImages.push_back(
                    {declaration.variable, sign,
                     boundExpression(state, environment, positiveObjective)
                         .upper(),
                     boundExpression(state, environment, negativeObjective)
                         .upper()});
            }
        }

        forget(state, targetDimension);
        if (assigned.upper().isFinite())
        {
            LinearExpression upper(target);
            upper.setConstant(-assigned.upper().value());
            addLessEqual(state, environment, upper, assigned.upper().isStrict(),
                         false);
        }
        if (assigned.lower().isFinite())
        {
            LinearExpression lower;
            lower.setCoefficient(target, Rational(-1));
            lower.setConstant(assigned.lower().value());
            addLessEqual(state, environment, lower, assigned.lower().isStrict(),
                         false);
        }
        for (const RelationalImage& image : relationalImages)
        {
            if (image.targetUpper.isFinite())
            {
                LinearExpression upper(target);
                upper.setCoefficient(image.variable, Rational(image.sign));
                upper.setConstant(-image.targetUpper.value());
                addLessEqual(state, environment, upper,
                             image.targetUpper.isStrict(), false);
            }
            if (image.targetLower.isFinite())
            {
                LinearExpression lower;
                lower.setCoefficient(target, Rational(-1));
                lower.setCoefficient(image.variable, Rational(image.sign));
                lower.setConstant(-image.targetLower.value());
                addLessEqual(state, environment, lower,
                             image.targetLower.isStrict(), false);
            }
        }
        normalize(state, environment);
        return ApproximationKind::SoundOverApproximation;
    }

    ApproximationKind assume(OctagonStorage& genericState,
                             const VariableEnvironment& environment,
                             const LinearConstraint& constraint) const
    {
        OctagonStorage& state = asOctagon(genericState);
        requireVariables(environment, constraint.expression());
        for (const auto& [variable, coefficient] :
             constraint.expression().terms())
        {
            (void)coefficient;
            if (environment.typeOf(variable).kind == NumericKind::IEEEFloat)
                return ApproximationKind::UnsupportedFallback;
        }

        const bool exact = addConstraint(state, environment, constraint);
        normalize(state, environment);
        return exact ? ApproximationKind::Exact
                     : ApproximationKind::SoundOverApproximation;
    }

    void forget(OctagonStorage& genericState, const VariableEnvironment& environment,
                Variable variable) const
    {
        if (!environment.contains(variable))
            throw std::invalid_argument(
                "forgotten variable is not in relational environment");
        forget(asOctagon(genericState), environment.dimensionOf(variable));
    }

    std::unique_ptr<OctagonStorage> join(const OctagonStorage& genericLhs,
                                      const OctagonStorage& genericRhs) const
    {
        std::optional<OctagonStorage> lhsStorage;
        std::optional<OctagonStorage> rhsStorage;
        const OctagonStorage& lhs =
            normalized(asOctagon(genericLhs), lhsStorage);
        const OctagonStorage& rhs =
            normalized(asOctagon(genericRhs), rhsStorage);
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return std::make_unique<OctagonStorage>(rhs.converted(lhs.kind()));
        if (rhs.bottom)
            return lhs.clone();

        auto result = std::make_unique<OctagonStorage>(
            lhs.variableKinds, lhs.kind(), OctagonStorage::ScratchMatrixTag{});
        for (std::size_t row = 0; row < result->nodes(); ++row)
            for (std::size_t column = 0; column < result->nodes(); ++column)
            {
                const Bound& left = lhs.at(row, column);
                const Bound& right = rhs.at(row, column);
                result->set(row, column, left <= right ? right : left);
            }

        // Point-wise maximum preserves coherence and every closure inequality.
        // If R[i,j] comes from operand K, then
        // K[i,j] <= K[i,h] + K[h,j] <= R[i,h] + R[h,j]; the same monotonicity
        // argument applies to the strong-closure inequality. Integer-tight
        // unary entries remain even because the maximum of two even bounds is
        // even. APRON's oct_join uses this same closed-result fast path instead
        // of scheduling another cubic close.
        result->stronglyClosed = true;
        return result;
    }

    std::unique_ptr<OctagonStorage> meet(const OctagonStorage& genericLhs,
                                      const OctagonStorage& genericRhs) const
    {
        std::optional<OctagonStorage> lhsStorage;
        std::optional<OctagonStorage> rhsStorage;
        const OctagonStorage& lhs =
            normalized(asOctagon(genericLhs), lhsStorage);
        const OctagonStorage& rhs =
            normalized(asOctagon(genericRhs), rhsStorage);
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return lhs.clone();
        if (rhs.bottom)
            return rhs.clone();

        auto result = std::make_unique<OctagonStorage>(
            lhs.variableKinds, lhs.kind(), OctagonStorage::ScratchMatrixTag{});
        for (std::size_t row = 0; row < result->nodes(); ++row)
            for (std::size_t column = 0; column < result->nodes(); ++column)
            {
                const Bound& left = lhs.at(row, column);
                const Bound& right = rhs.at(row, column);
                result->set(row, column, left <= right ? left : right);
            }
        result->stronglyClosed = false;
        normalize(*result);
        return result;
    }

    std::unique_ptr<OctagonStorage> widen(const OctagonStorage& genericCurrent,
                                       const OctagonStorage& genericNext,
                                       const WideningPolicy& policy) const
    {
        const OctagonStorage& current = asOctagon(genericCurrent);
        std::optional<OctagonStorage> nextStorage;
        const OctagonStorage& next =
            normalized(asOctagon(genericNext), nextStorage);
        requireSameSize(current, next);
        if (current.bottom)
            return std::make_unique<OctagonStorage>(
                next.converted(current.kind()));
        if (next.bottom)
            return current.clone();

        std::vector<Rational> thresholds = policy.thresholds;
        std::sort(thresholds.begin(), thresholds.end());

        auto result = std::make_unique<OctagonStorage>(
            current.variableKinds, current.kind(),
            OctagonStorage::ScratchMatrixTag{});
        for (std::size_t row = 0; row < result->nodes(); ++row)
        {
            for (std::size_t column = 0; column < result->nodes(); ++column)
            {
                const Bound& oldBound = current.at(row, column);
                const Bound& nextBound = next.at(row, column);
                if (nextBound <= oldBound)
                {
                    result->set(row, column, oldBound);
                    continue;
                }

                result->set(row, column, Bound::plusInfinity());
                if (nextBound.isFinite())
                {
                    // Public thresholds use normalized octagonal constants.
                    // A unary DBM entry encodes +/-2*x, so its matrix bound
                    // needs twice the user-visible threshold.
                    const bool unary = row != column && row / 2 == column / 2;
                    for (const Rational& threshold : thresholds)
                    {
                        Bound candidate = Bound::finite(
                            unary ? threshold * Rational(2) : threshold);
                        if (nextBound <= candidate)
                        {
                            result->set(row, column, candidate);
                            break;
                        }
                    }
                }
            }
        }
        for (std::size_t node = 0; node < result->nodes(); ++node)
            result->set(node, node, Bound::finite(Rational()));
        result->stronglyClosed = false;
        return result;
    }

    std::unique_ptr<OctagonStorage> narrow(const OctagonStorage& genericCurrent,
                                        const OctagonStorage& genericNext) const
    {
        std::optional<OctagonStorage> currentStorage;
        std::optional<OctagonStorage> nextStorage;
        const OctagonStorage& current =
            normalized(asOctagon(genericCurrent), currentStorage);
        const OctagonStorage& next =
            normalized(asOctagon(genericNext), nextStorage);
        requireSameSize(current, next);
        if (current.bottom || next.bottom)
            return next.clone();

        auto result = std::make_unique<OctagonStorage>(current);
        for (std::size_t row = 0; row < result->nodes(); ++row)
            for (std::size_t column = 0; column < result->nodes(); ++column)
                if (current.at(row, column).isPlusInfinity() &&
                    next.at(row, column).isFinite())
                    result->set(row, column, next.at(row, column));
        result->stronglyClosed = false;
        return result;
    }

    std::unique_ptr<OctagonStorage> projectLowerBounds(
        const OctagonStorage& genericState) const
    {
        std::optional<OctagonStorage> sourceStorage;
        const OctagonStorage& source =
            normalized(asOctagon(genericState), sourceStorage);
        if (source.bottom)
            return source.clone();

        auto result = std::make_unique<OctagonStorage>(
            source.variableKinds, source.kind());
        for (Dimension dimension = 0; dimension < source.dimensions;
             ++dimension)
        {
            setCoherent(
                *result, negativeNode(dimension), positiveNode(dimension),
                source.at(negativeNode(dimension), positiveNode(dimension)));
        }
        for (Dimension lhs = 0; lhs < source.dimensions; ++lhs)
        {
            for (Dimension rhs = lhs + 1; rhs < source.dimensions; ++rhs)
            {
                // -(x+y) and -(x-y) are the selected lower orientations.
                setCoherent(*result, negativeNode(lhs), positiveNode(rhs),
                            source.at(negativeNode(lhs), positiveNode(rhs)));
                setCoherent(*result, negativeNode(lhs), negativeNode(rhs),
                            source.at(negativeNode(lhs), negativeNode(rhs)));
            }
        }
        result->stronglyClosed = false;
        normalize(*result);
        return result;
    }

    std::unique_ptr<OctagonStorage> changeEnvironment(
        const OctagonStorage& genericState, const VariableEnvironment& oldEnvironment,
        const VariableEnvironment& newEnvironment, bool initializeNewVariablesToZero) const
    {
        std::optional<OctagonStorage> sourceStorage;
        const OctagonStorage& source =
            normalized(asOctagon(genericState), sourceStorage);
        if (source.dimensions != oldEnvironment.size())
            throw std::invalid_argument(
                "old environment does not match octagon dimensions");
        if (source.bottom)
            return std::make_unique<OctagonStorage>(newEnvironment,
                                                    source.kind(), true);

        auto result = std::make_unique<OctagonStorage>(newEnvironment,
                                                       source.kind());
        for (Dimension newRowDimension = 0;
             newRowDimension < newEnvironment.size(); ++newRowDimension)
        {
            const Variable rowVariable =
                newEnvironment.variableOf(newRowDimension);
            if (!oldEnvironment.contains(rowVariable))
                continue;
            if (oldEnvironment.typeOf(rowVariable) !=
                newEnvironment.typeOf(rowVariable))
                throw std::invalid_argument(
                    "environment change cannot alter a variable's type");
            const Dimension oldRowDimension =
                oldEnvironment.dimensionOf(rowVariable);

            for (Dimension newColumnDimension = 0;
                 newColumnDimension < newEnvironment.size();
                 ++newColumnDimension)
            {
                const Variable columnVariable =
                    newEnvironment.variableOf(newColumnDimension);
                if (!oldEnvironment.contains(columnVariable))
                    continue;
                if (oldEnvironment.typeOf(columnVariable) !=
                    newEnvironment.typeOf(columnVariable))
                    throw std::invalid_argument(
                        "environment change cannot alter a variable's type");
                const Dimension oldColumnDimension =
                    oldEnvironment.dimensionOf(columnVariable);

                for (std::size_t rowSign = 0; rowSign < 2; ++rowSign)
                {
                    for (std::size_t columnSign = 0; columnSign < 2;
                         ++columnSign)
                    {
                        result->set(
                            2 * newRowDimension + rowSign,
                            2 * newColumnDimension + columnSign,
                            source.at(2 * oldRowDimension + rowSign,
                                      2 * oldColumnDimension + columnSign));
                    }
                }
            }
        }

        if (initializeNewVariablesToZero)
        {
            for (Dimension dimension = 0; dimension < newEnvironment.size();
                 ++dimension)
            {
                const Variable value = newEnvironment.variableOf(dimension);
                if (oldEnvironment.contains(value))
                    continue;
                setCoherent(*result, positiveNode(dimension),
                            negativeNode(dimension), Bound::finite(Rational()));
                setCoherent(*result, negativeNode(dimension),
                            positiveNode(dimension), Bound::finite(Rational()));
            }
        }
        result->stronglyClosed = false;
        normalize(*result);
        return result;
    }

    bool isBottom(const OctagonStorage& genericState) const
    {
        std::optional<OctagonStorage> storage;
        return normalized(asOctagon(genericState), storage).bottom;
    }

    bool isTop(const OctagonStorage& genericState) const
    {
        std::optional<OctagonStorage> storage;
        const OctagonStorage& state =
            normalized(asOctagon(genericState), storage);
        if (state.bottom)
            return false;
        for (std::size_t row = 0; row < state.nodes(); ++row)
        {
            for (std::size_t column = 0; column < state.nodes(); ++column)
            {
                if (row == column)
                    continue;
                if (!state.at(row, column).isPlusInfinity())
                    return false;
            }
        }
        return true;
    }

    bool leq(const OctagonStorage& genericLhs, const OctagonStorage& genericRhs) const
    {
        std::optional<OctagonStorage> lhsStorage;
        std::optional<OctagonStorage> rhsStorage;
        const OctagonStorage& lhs =
            normalized(asOctagon(genericLhs), lhsStorage);
        const OctagonStorage& rhs =
            normalized(asOctagon(genericRhs), rhsStorage);
        requireSameSize(lhs, rhs);
        if (lhs.bottom)
            return true;
        if (rhs.bottom)
            return false;
        for (std::size_t row = 0; row < lhs.nodes(); ++row)
            for (std::size_t column = 0; column < lhs.nodes(); ++column)
                if (!(lhs.at(row, column) <= rhs.at(row, column)))
                    return false;
        return true;
    }

    Interval bound(const OctagonStorage& genericState,
                   const VariableEnvironment& environment, Variable variable) const
    {
        if (!environment.contains(variable))
            throw std::invalid_argument(
                "bounded variable is not in relational environment");
        std::optional<OctagonStorage> storage;
        const OctagonStorage& state =
            normalized(asOctagon(genericState), storage);
        if (state.bottom)
            return Interval(Bound::plusInfinity(), Bound::minusInfinity());

        const Dimension dimension = environment.dimensionOf(variable);
        const Bound& doubledUpper =
            state.at(positiveNode(dimension), negativeNode(dimension));
        const Bound& doubledNegativeLower =
            state.at(negativeNode(dimension), positiveNode(dimension));

        Bound lower = Bound::minusInfinity();
        Bound upper = Bound::plusInfinity();
        if (doubledUpper.isFinite())
            upper = Bound::divideByPositive(doubledUpper, Rational(2));
        if (doubledNegativeLower.isFinite())
        {
            lower = Bound::finite(-(doubledNegativeLower.value() / Rational(2)),
                                  doubledNegativeLower.isStrict());
        }
        return Interval(std::move(lower), std::move(upper));
    }

    LinearConstraintSet constraints(const OctagonStorage& genericState,
                                    const VariableEnvironment& environment) const
    {
        std::optional<OctagonStorage> storage;
        const OctagonStorage& state =
            normalized(asOctagon(genericState), storage);
        LinearConstraintSet result;
        if (state.bottom)
            return result;

        for (std::size_t row = 0; row < state.nodes(); ++row)
        {
            for (std::size_t column = 0; column < state.nodes(); ++column)
            {
                if (row == column || !state.at(row, column).isFinite())
                    continue;

                // Coherent entries describe the same octagonal constraint.
                const std::size_t coherentRow = opposite(column);
                const std::size_t coherentColumn = opposite(row);
                if (std::pair<std::size_t, std::size_t>(coherentRow,
                                                        coherentColumn) <
                    std::pair<std::size_t, std::size_t>(row, column))
                    continue;

                LinearExpression expression;
                addNodeTerm(expression, environment, row, Rational(1));
                addNodeTerm(expression, environment, column, Rational(-1));
                expression.setConstant(-state.at(row, column).value());
                result.emplace_back(std::move(expression),
                                    state.at(row, column).isStrict()
                                        ? ConstraintKind::LessThan
                                        : ConstraintKind::LessEqual);
            }
        }
        return result;
    }

    std::string toString(const OctagonStorage& genericState,
                         const VariableEnvironment& environment) const
    {
        if (isBottom(genericState))
            return "bottom";
        const LinearConstraintSet exported =
            constraints(genericState, environment);
        if (exported.empty())
            return "top";
        std::ostringstream output;
        for (std::size_t index = 0; index < exported.size(); ++index)
        {
            if (index != 0)
                output << " && ";
            output << exported[index].toString(&environment);
        }
        return output.str();
    }

    const OctagonConfig& config() const
    {
        return options_;
    }

    OctagonStorageStats storageStats() const
    {
        const OctagonShape shape = measureShape(state_);
        return {state_.kind(), shape.dimensions, shape.finiteStored,
                shape.allocatedSlots, shape.components,
                shape.maximumComponent};
    }

    void reconfigure(OctagonConfig options)
    {
        if (state_.kind() != options.storage)
            state_ = state_.converted(options.storage);
        options_ = std::move(options);
        // A matrix marked closed under a weaker policy must be normalized
        // again when stronger integer/strong closure is enabled.
        if (!state_.bottom)
        {
            state_.stronglyClosed = false;
            normalize(state_);
        }
    }

    ApproximationKind assignCurrent(const VariableEnvironment& environment,
                                    Variable target,
                                    const LinearExpression& expression)
    {
        StorageTelemetryScope telemetry("abstract", "assign", state_);
        return assign(state_, environment, target, expression);
    }

    ApproximationKind assumeCurrent(const VariableEnvironment& environment,
                                    const LinearConstraint& constraint)
    {
        StorageTelemetryScope telemetry("abstract", "assume", state_);
        return assume(state_, environment, constraint);
    }

    void forgetCurrent(const VariableEnvironment& environment, Variable variable)
    {
        StorageTelemetryScope telemetry("abstract", "forget", state_, true);
        forget(state_, environment, variable);
    }

    void assignIntervalCurrent(const VariableEnvironment& environment,
                               Variable target, const Interval& value)
    {
        StorageTelemetryScope telemetry("abstract", "assign-interval", state_);
        if (!environment.contains(target))
            throw std::invalid_argument(
                "assignment target is not in relational environment");
        if (state_.bottom)
            return;
        const Dimension dimension = environment.dimensionOf(target);
        forget(state_, dimension);
        if (value.isBottom())
        {
            state_.bottom = true;
            return;
        }
        if (value.upper().isFinite())
        {
            LinearExpression upper(target);
            upper.setConstant(-value.upper().value());
            addLessEqual(state_, environment, upper,
                         value.upper().isStrict(), false);
        }
        if (value.lower().isFinite())
        {
            LinearExpression lower;
            lower.setCoefficient(target, Rational(-1));
            lower.setConstant(value.lower().value());
            addLessEqual(state_, environment, lower,
                         value.lower().isStrict(), false);
        }
        normalize(state_, environment);
    }

    void canonicalizeCurrent()
    {
        StorageTelemetryScope telemetry("abstract", "canonicalize", state_);
        normalize(state_);
    }

    void joinCurrent(const Impl& other)
    {
        StorageTelemetryScope telemetry("abstract", "join", state_, true);
        state_ = std::move(*join(state_, other.state_));
    }

    std::unique_ptr<Impl> joined(const Impl& other) const
    {
        StorageTelemetryScope telemetry("abstract", "join", state_);
        return std::make_unique<Impl>(
            options_, std::move(*join(state_, other.state_)));
    }

    void meetCurrent(const Impl& other)
    {
        StorageTelemetryScope telemetry("abstract", "meet", state_, true);
        state_ = std::move(*meet(state_, other.state_));
    }

    std::unique_ptr<Impl> met(const Impl& other) const
    {
        StorageTelemetryScope telemetry("abstract", "meet", state_);
        return std::make_unique<Impl>(
            options_, std::move(*meet(state_, other.state_)));
    }

    void widenCurrent(const Impl& other, const WideningPolicy& policy)
    {
        StorageTelemetryScope telemetry("abstract", "widen", state_, true);
        state_ = std::move(*widen(state_, other.state_, policy));
    }

    std::unique_ptr<Impl> widened(
        const Impl& other, const WideningPolicy& policy) const
    {
        StorageTelemetryScope telemetry("abstract", "widen", state_);
        return std::make_unique<Impl>(
            options_, std::move(*widen(state_, other.state_, policy)));
    }

    void narrowCurrent(const Impl& other)
    {
        StorageTelemetryScope telemetry("abstract", "narrow", state_, true);
        state_ = std::move(*narrow(state_, other.state_));
    }

    std::unique_ptr<Impl> narrowed(const Impl& other) const
    {
        StorageTelemetryScope telemetry("abstract", "narrow", state_);
        return std::make_unique<Impl>(
            options_, std::move(*narrow(state_, other.state_)));
    }

    void projectLowerBoundsCurrent()
    {
        StorageTelemetryScope telemetry(
            "abstract", "project-lower-bounds", state_, true);
        state_ = std::move(*projectLowerBounds(state_));
    }

    std::unique_ptr<Impl> projectedLowerBounds() const
    {
        StorageTelemetryScope telemetry(
            "abstract", "project-lower-bounds", state_);
        return std::make_unique<Impl>(
            options_, std::move(*projectLowerBounds(state_)));
    }

    void changeEnvironmentCurrent(const VariableEnvironment& oldEnvironment,
                                  const VariableEnvironment& newEnvironment,
                                  bool initializeNewVariablesToZero)
    {
        StorageTelemetryScope telemetry(
            "abstract", "change-environment", state_, true);
        state_ = std::move(*changeEnvironment(
            state_, oldEnvironment, newEnvironment, initializeNewVariablesToZero));
    }

    std::unique_ptr<Impl> changedEnvironment(
        const VariableEnvironment& oldEnvironment, const VariableEnvironment& newEnvironment,
        bool initializeNewVariablesToZero) const
    {
        StorageTelemetryScope telemetry(
            "abstract", "change-environment", state_);
        return std::make_unique<Impl>(
            options_, std::move(*changeEnvironment(
                          state_, oldEnvironment, newEnvironment,
                          initializeNewVariablesToZero)));
    }

    bool isBottomCurrent() const
    {
        StorageTelemetryScope telemetry("query", "is-bottom", state_);
        return isBottom(state_);
    }

    bool isTopCurrent() const
    {
        StorageTelemetryScope telemetry("query", "is-top", state_);
        return isTop(state_);
    }

    bool leqCurrent(const Impl& other) const
    {
        StorageTelemetryScope telemetry("query", "leq", state_);
        return leq(state_, other.state_);
    }

    Interval boundCurrent(const VariableEnvironment& environment,
                          Variable variable) const
    {
        StorageTelemetryScope telemetry("query", "bound-variable", state_);
        return bound(state_, environment, variable);
    }

    Interval boundExpressionCurrent(
        const VariableEnvironment& environment,
        const LinearExpression& expression) const
    {
        StorageTelemetryScope telemetry("query", "bound-expression", state_);
        return boundExpression(state_, environment, expression);
    }

    LinearConstraintSet constraintsCurrent(
        const VariableEnvironment& environment) const
    {
        StorageTelemetryScope telemetry("query", "to-constraints", state_);
        return constraints(state_, environment);
    }

    std::string toStringCurrent(const VariableEnvironment& environment) const
    {
        StorageTelemetryScope telemetry("query", "to-string", state_);
        return toString(state_, environment);
    }

private:
    void requireSameSize(const OctagonStorage& lhs, const OctagonStorage& rhs) const
    {
        if (lhs.dimensions != rhs.dimensions ||
            lhs.variableKinds != rhs.variableKinds)
            throw std::invalid_argument("octagon state shapes do not match");
    }

    void requireVariables(const VariableEnvironment& environment,
                          const LinearExpression& expression) const
    {
        for (const auto& [variable, coefficient] : expression.terms())
        {
            (void)coefficient;
            if (!environment.contains(variable))
                throw std::invalid_argument(
                    "expression variable is not in relational environment");
        }
    }

    void setCoherent(OctagonStorage& state, std::size_t row, std::size_t column,
                     const Bound& bound) const
    {
        state.set(row, column, Bound::min(state.at(row, column), bound));
        const std::size_t coherentRow = opposite(column);
        const std::size_t coherentColumn = opposite(row);
        state.set(coherentRow, coherentColumn,
                  Bound::min(state.at(coherentRow, coherentColumn), bound));
        state.stronglyClosed = false;
    }

    void forget(OctagonStorage& state, Dimension dimension) const
    {
        if (dimension >= state.dimensions)
            throw std::out_of_range("invalid forgotten octagon dimension");
        if (state.bottom)
            return;
        normalize(state);
        const std::size_t first = positiveNode(dimension);
        const std::size_t second = negativeNode(dimension);
        for (std::size_t node = 0; node < state.nodes(); ++node)
        {
            state.set(first, node, Bound::plusInfinity());
            state.set(second, node, Bound::plusInfinity());
            state.set(node, first, Bound::plusInfinity());
            state.set(node, second, Bound::plusInfinity());
        }
        state.set(first, first, Bound::finite(Rational()));
        state.set(second, second, Bound::finite(Rational()));
        state.compactComponents();
        state.stronglyClosed = true;
    }

    bool addConstraint(OctagonStorage& state, const VariableEnvironment& environment,
                       const LinearConstraint& constraint) const
    {
        if (state.bottom)
            return true;

        switch (constraint.kind())
        {
        case ConstraintKind::Equal: {
            const bool forward = addLessEqual(state, environment,
                                              constraint.expression(), false);
            const bool backward = addLessEqual(state, environment,
                                               -constraint.expression(), false);
            return forward && backward;
        }
        case ConstraintKind::NotEqual:
            return handleConstantNotEqual(state, constraint.expression());
        case ConstraintKind::LessThan:
            return addLessEqual(state, environment, constraint.expression(),
                                true);
        case ConstraintKind::LessEqual:
            return addLessEqual(state, environment, constraint.expression(),
                                false);
        case ConstraintKind::GreaterThan:
            return addLessEqual(state, environment, -constraint.expression(),
                                true);
        case ConstraintKind::GreaterEqual:
            return addLessEqual(state, environment, -constraint.expression(),
                                false);
        }
        return false;
    }

    bool handleConstantNotEqual(OctagonStorage& state,
                                const LinearExpression& expression) const
    {
        if (!expression.terms().empty())
            return false;
        if (expression.constant().isZero())
            state.bottom = true;
        return true;
    }

    bool addLessEqual(OctagonStorage& state, const VariableEnvironment& environment,
                      const LinearExpression& expression, bool strict,
                      bool allowLinearization = true) const
    {
        const auto& terms = expression.terms();
        if (terms.empty())
        {
            const int sign = expression.constant().sign();
            if (sign > 0 || (sign == 0 && strict))
                state.bottom = true;
            return true;
        }

        if (terms.size() == 1)
        {
            const auto& [variable, coefficient] = *terms.begin();
            if (coefficient.isZero())
                return false;
            const Dimension dimension = environment.dimensionOf(variable);
            const Rational magnitude = absolute(coefficient);
            const Rational value = -expression.constant() / magnitude;
            const std::size_t row = coefficient.sign() > 0
                                        ? positiveNode(dimension)
                                        : negativeNode(dimension);
            const std::size_t column = opposite(row);
            setCoherent(state, row, column,
                        Bound::finite(value * Rational(2), strict));
            return true;
        }

        if (terms.size() == 2)
        {
            auto it = terms.begin();
            const Variable lhsVariable = it->first;
            const Rational lhsCoefficient = it->second;
            ++it;
            const Variable rhsVariable = it->first;
            const Rational rhsCoefficient = it->second;
            const Rational lhsMagnitude = absolute(lhsCoefficient);
            if (lhsMagnitude != absolute(rhsCoefficient) ||
                lhsMagnitude.isZero())
            {
                if (allowLinearization)
                    addLinearized(state, environment, expression, strict);
                return false;
            }

            const Dimension lhsDimension = environment.dimensionOf(lhsVariable);
            const Dimension rhsDimension = environment.dimensionOf(rhsVariable);
            const std::size_t lhsNode = lhsCoefficient.sign() > 0
                                            ? positiveNode(lhsDimension)
                                            : negativeNode(lhsDimension);
            const std::size_t negativeRhsNode =
                rhsCoefficient.sign() > 0 ? negativeNode(rhsDimension)
                                          : positiveNode(rhsDimension);
            const Rational value = -expression.constant() / lhsMagnitude;
            setCoherent(state, lhsNode, negativeRhsNode,
                        Bound::finite(value, strict));
            return true;
        }

        if (allowLinearization)
            addLinearized(state, environment, expression, strict);
        return false;
    }

    /// Interval of a linear expression under the current per-variable bounds.
    Interval evaluateInterval(const OctagonStorage& state,
                              const VariableEnvironment& environment,
                              const LinearExpression& expression) const
    {
        Rational low = expression.constant();
        Rational high = expression.constant();
        bool lowFinite = true;
        bool highFinite = true;
        bool lowStrict = false;
        bool highStrict = false;
        for (const auto& [variable, coefficient] : expression.terms())
        {
            if (coefficient.isZero())
                continue;
            const Interval interval = bound(state, environment, variable);
            const Bound& least =
                coefficient.sign() > 0 ? interval.lower() : interval.upper();
            const Bound& greatest =
                coefficient.sign() > 0 ? interval.upper() : interval.lower();
            if (lowFinite && least.isFinite())
            {
                low += least.value() * coefficient;
                lowStrict = lowStrict || least.isStrict();
            }
            else
            {
                lowFinite = false;
            }
            if (highFinite && greatest.isFinite())
            {
                high += greatest.value() * coefficient;
                highStrict = highStrict || greatest.isStrict();
            }
            else
            {
                highFinite = false;
            }
        }
        return Interval(lowFinite ? Bound::finite(low, lowStrict)
                                  : Bound::minusInfinity(),
                        highFinite ? Bound::finite(high, highStrict)
                                   : Bound::plusInfinity());
    }

    Interval boundExpression(const OctagonStorage& state,
                             const VariableEnvironment& environment,
                             const LinearExpression& expression) const
    {
        requireVariables(environment, expression);
        if (state.bottom)
            return Interval(Bound::plusInfinity(), Bound::minusInfinity());
        if (expression.terms().size() != 2)
            return evaluateInterval(state, environment, expression);

        auto term = expression.terms().begin();
        const Variable firstVariable = term->first;
        const Rational firstCoefficient = term->second;
        ++term;
        const Variable secondVariable = term->first;
        const Rational secondCoefficient = term->second;
        const Rational magnitude = absolute(firstCoefficient);
        if (magnitude.isZero() ||
            magnitude != absolute(secondCoefficient))
            return evaluateInterval(state, environment, expression);

        std::optional<OctagonStorage> normalizedStorage;
        const OctagonStorage& source = normalized(state, normalizedStorage);
        const auto signedSumUpper = [&](bool negate) -> Bound
        {
            const int firstSign =
                (negate ? -firstCoefficient : firstCoefficient).sign();
            const int secondSign =
                (negate ? -secondCoefficient : secondCoefficient).sign();
            const std::size_t row =
                firstSign > 0
                    ? positiveNode(environment.dimensionOf(firstVariable))
                    : negativeNode(environment.dimensionOf(firstVariable));
            const std::size_t column =
                secondSign > 0
                    ? negativeNode(environment.dimensionOf(secondVariable))
                    : positiveNode(environment.dimensionOf(secondVariable));
            return source.at(row, column);
        };

        const Bound positive = signedSumUpper(false);
        const Bound negative = signedSumUpper(true);
        const Bound lower = negative.isFinite()
                                ? Bound::finite(
                                      expression.constant() -
                                          magnitude * negative.value(),
                                      negative.isStrict())
                                : Bound::minusInfinity();
        const Bound upper = positive.isFinite()
                                ? Bound::finite(
                                      expression.constant() +
                                          magnitude * positive.value(),
                                      positive.isStrict())
                                : Bound::plusInfinity();
        return Interval(lower, upper);
    }

    /// Strongest octagonal consequences of a constraint that is not itself
    /// octagonal.
    ///
    /// A DBM cannot store `sum a_i x_i + k <= 0` once it has three terms, or
    /// two terms of different magnitude. Dropping it is sound and needlessly
    /// weak: replacing the terms that cannot be stored by their current bounds
    /// leaves a constraint over one or two variables that can be. That is
    /// Mine's interval linearization, restricted to the sub-expressions this
    /// domain represents exactly, and it is what makes a guard such as
    /// `2*i + 3*j <= 10` narrow anything at all here.
    void addLinearized(OctagonStorage& state,
                       const VariableEnvironment& environment,
                       const LinearExpression& expression, bool strict) const
    {
        normalize(state, environment);
        if (state.bottom)
            return;

        std::vector<Variable> variables;
        for (const auto& [variable, coefficient] : expression.terms())
        {
            if (!coefficient.isZero())
                variables.push_back(variable);
        }
        if (variables.size() < 2)
            return;

        // Derive every bound from the same pre-constraint snapshot, so the
        // result does not depend on the order the sub-constraints are applied.
        std::map<Variable, Interval> intervals;
        for (Variable variable : variables)
            intervals.emplace(variable, bound(state, environment, variable));

        // Least value the terms outside `kept` can take.
        const auto restMinimum = [&](const std::vector<Variable>& kept,
                                     Rational& total, bool& totalStrict)
        {
            total = Rational();
            totalStrict = false;
            for (const auto& [variable, coefficient] : expression.terms())
            {
                if (coefficient.isZero())
                    continue;
                if (std::find(kept.begin(), kept.end(), variable) != kept.end())
                    continue;
                const Interval& interval = intervals.at(variable);
                const Bound& endpoint = coefficient.sign() > 0
                                            ? interval.lower()
                                            : interval.upper();
                if (!endpoint.isFinite())
                    return false;
                total += endpoint.value() * coefficient;
                totalStrict = totalStrict || endpoint.isStrict();
            }
            return true;
        };

        const auto applyUnary = [&](Variable kept)
        {
            Rational rest;
            bool restStrict = false;
            if (!restMinimum({kept}, rest, restStrict))
                return;
            LinearExpression reduced(expression.constant() + rest);
            reduced.setCoefficient(kept, expression.coefficient(kept));
            addLessEqual(state, environment, reduced, strict || restStrict,
                         false);
        };

        // For a pair a*x+b*y, retain the largest common octagonal part
        // m*(sign(a)*x+sign(b)*y), where m=min(|a|,|b|), and intervalize only
        // the coefficient remainders.  When |a|=|b| this is the exact pair
        // handled previously.  Unequal magnitudes now produce the strongest
        // consequence available from this interval decomposition, and the
        // construction is invariant under positive scaling of the input row.
        const auto applyPair = [&](Variable first, Variable second)
        {
            const Rational firstCoefficient = expression.coefficient(first);
            const Rational secondCoefficient = expression.coefficient(second);
            const Rational common =
                std::min(absolute(firstCoefficient),
                         absolute(secondCoefficient));
            if (common.isZero())
                return;

            Rational rest = expression.constant();
            bool restStrict = false;
            for (const auto& [variable, coefficient] : expression.terms())
            {
                Rational remainder = coefficient;
                if (variable == first)
                    remainder -= Rational(firstCoefficient.sign()) * common;
                else if (variable == second)
                    remainder -= Rational(secondCoefficient.sign()) * common;
                if (remainder.isZero())
                    continue;
                const Interval& interval = intervals.at(variable);
                const Bound& endpoint = remainder.sign() > 0
                                            ? interval.lower()
                                            : interval.upper();
                if (!endpoint.isFinite())
                    return;
                rest += remainder * endpoint.value();
                restStrict = restStrict || endpoint.isStrict();
            }

            LinearExpression reduced(rest);
            reduced.setCoefficient(
                first, Rational(firstCoefficient.sign()) * common);
            reduced.setCoefficient(
                second, Rational(secondCoefficient.sign()) * common);
            addLessEqual(state, environment, reduced, strict || restStrict,
                         false);
        };

        for (std::size_t first = 0; first < variables.size(); ++first)
        {
            applyUnary(variables[first]);
            for (std::size_t second = first + 1; second < variables.size();
                 ++second)
                applyPair(variables[first], variables[second]);
        }
    }

    void selfAssign(OctagonStorage& state, const VariableEnvironment& environment,
                    Dimension target, int sign, const Rational& constant) const
    {
        normalize(state, environment);
        if (state.bottom)
            return;
        OctagonStorage old = std::move(state);
        OctagonStorage result(old.variableKinds, old.kind(),
                              OctagonStorage::ScratchMatrixTag{});

        auto oldNode = [target, sign](std::size_t newNode) {
            if (newNode == positiveNode(target))
                return sign > 0 ? positiveNode(target) : negativeNode(target);
            if (newNode == negativeNode(target))
                return sign > 0 ? negativeNode(target) : positiveNode(target);
            return newNode;
        };
        auto delta = [target, &constant](std::size_t newNode) {
            if (newNode == positiveNode(target))
                return constant;
            if (newNode == negativeNode(target))
                return -constant;
            return Rational();
        };

        for (std::size_t row = 0; row < result.nodes(); ++row)
        {
            for (std::size_t column = 0; column < result.nodes(); ++column)
            {
                const Bound& oldBound = old.at(oldNode(row), oldNode(column));
                if (!oldBound.isFinite())
                    result.set(row, column, oldBound);
                else
                    result.set(row, column,
                               Bound::finite(oldBound.value() + delta(row) -
                                                 delta(column),
                                             oldBound.isStrict()));
            }
        }
        result.stronglyClosed = old.stronglyClosed;
        state = std::move(result);
    }

    void enforceCoherence(OctagonStorage& state) const
    {
        for (std::size_t row = 0; row < state.nodes(); ++row)
        {
            for (std::size_t column = 0; column < state.nodes(); ++column)
            {
                const Bound coherent =
                    state.at(opposite(column), opposite(row));
                const Bound minimum =
                    Bound::min(state.at(row, column), coherent);
                state.set(row, column, minimum);
                state.set(opposite(column), opposite(row), minimum);
            }
        }
    }

    void shortestPathClosure(OctagonStorage& state) const
    {
        StorageTelemetryScope telemetry(
            "primitive", "shortest-path-closure", state);
        for (std::size_t middle = 0; middle < state.nodes(); ++middle)
        {
            for (std::size_t row = 0; row < state.nodes(); ++row)
            {
                if (state.at(row, middle).isPlusInfinity())
                    continue;
                for (std::size_t column = 0; column < state.nodes(); ++column)
                {
                    if (state.at(middle, column).isPlusInfinity())
                        continue;
                    const Bound candidate = Bound::add(
                        state.at(row, middle), state.at(middle, column));
                    state.set(row, column,
                              Bound::min(state.at(row, column), candidate));
                }
            }
        }
    }

    void integerTighten(OctagonStorage& state) const
    {
        if (!options_.integerTightening)
            return;
        for (Dimension dimension = 0; dimension < state.dimensions; ++dimension)
        {
            if (state.variableKinds[dimension] != NumericKind::Integer)
                continue;
            const std::size_t positive = positiveNode(dimension);
            const std::size_t negative = negativeNode(dimension);
            state.set(positive, negative,
                      tightenIntegerUnary(state.at(positive, negative)));
            state.set(negative, positive,
                      tightenIntegerUnary(state.at(negative, positive)));
        }
    }

    void strongClosure(OctagonStorage& state) const
    {
        StorageTelemetryScope telemetry("primitive", "strong-closure", state);
        if (!options_.strongClosure)
            return;
        for (std::size_t row = 0; row < state.nodes(); ++row)
        {
            for (std::size_t column = 0; column < state.nodes(); ++column)
            {
                const Bound& lhs = state.at(row, opposite(row));
                const Bound& rhs = state.at(opposite(column), column);
                if (lhs.isPlusInfinity() || rhs.isPlusInfinity())
                    continue;
                const Bound candidate =
                    Bound::divideByPositive(Bound::add(lhs, rhs), Rational(2));
                state.set(row, column,
                          Bound::min(state.at(row, column), candidate));
            }
        }
    }

    bool detectBottom(OctagonStorage& state) const
    {
        for (std::size_t node = 0; node < state.nodes(); ++node)
        {
            if (isNegativeDiagonal(state.at(node, node)))
            {
                state.bottom = true;
                return true;
            }
        }
        return state.bottom;
    }

    void normalize(OctagonStorage& state) const
    {
        if (state.bottom || state.stronglyClosed)
        {
            StorageTelemetryScope telemetry(
                "primitive", "normalize-skip", state);
            return;
        }
        StorageTelemetryScope telemetry(
            "primitive", "normalize-dirty", state, true);
        enforceCoherence(state);
        shortestPathClosure(state);
        if (detectBottom(state))
            return;
        integerTighten(state);
        shortestPathClosure(state);
        if (detectBottom(state))
            return;
        strongClosure(state);
        enforceCoherence(state);
        if (detectBottom(state))
            return;
        state.stronglyClosed = true;
    }

    void normalize(OctagonStorage& state, const VariableEnvironment& environment) const
    {
        (void)environment;
        normalize(state);
    }

    const OctagonStorage& normalized(
        const OctagonStorage& state,
        std::optional<OctagonStorage>& normalizedStorage) const
    {
        if (state.bottom || state.stronglyClosed)
            return state;
        normalizedStorage.emplace(state);
        normalize(*normalizedStorage);
        return *normalizedStorage;
    }

    void addNodeTerm(LinearExpression& expression,
                     const VariableEnvironment& environment, std::size_t node,
                     const Rational& multiplier) const
    {
        const Dimension dimension = node / 2;
        const Rational sign = node % 2 == 0 ? Rational(1) : Rational(-1);
        const Variable variable = environment.variableOf(dimension);
        expression.setCoefficient(variable, expression.coefficient(variable) +
                                                sign * multiplier);
    }

    OctagonConfig options_;
    OctagonStorage state_;
};

OctagonState::OctagonState(VariableEnvironment environment, OctagonConfig config,
                           bool bottom)
    : environment_(std::move(environment)),
      impl_(std::make_unique<Impl>(environment_, std::move(config), bottom))
{
}

OctagonState::OctagonState(VariableEnvironment environment, std::unique_ptr<Impl> impl)
    : environment_(std::move(environment)), impl_(std::move(impl))
{
}

OctagonState OctagonState::top(const VariableEnvironment& environment,
                               const OctagonConfig& config)
{
    return OctagonState(environment, config, false);
}

OctagonState OctagonState::bottom(const VariableEnvironment& environment,
                                  const OctagonConfig& config)
{
    return OctagonState(environment, config, true);
}

OctagonState OctagonState::fromBox(const VariableEnvironment& environment,
                                   const IntervalBox& box,
                                   const OctagonConfig& config)
{
    OctagonState state = top(environment, config);
    for (const auto& [variable, interval] : box.bounds)
    {
        if (!environment.contains(variable))
            throw std::invalid_argument("box contains an unknown variable");
        if (interval.isBottom())
            return bottom(environment, config);
        if (interval.lower().isFinite())
        {
            LinearExpression expression(variable);
            expression.setConstant(-interval.lower().value());
            state.assume(LinearConstraint(
                std::move(expression),
                interval.lower().isStrict() ? ConstraintKind::GreaterThan
                                            : ConstraintKind::GreaterEqual));
        }
        if (interval.upper().isFinite())
        {
            LinearExpression expression(variable);
            expression.setConstant(-interval.upper().value());
            state.assume(LinearConstraint(
                std::move(expression),
                interval.upper().isStrict() ? ConstraintKind::LessThan
                                            : ConstraintKind::LessEqual));
        }
    }
    return state;
}

OctagonState OctagonState::fromConstraints(
    const VariableEnvironment& environment,
    const LinearConstraintSet& constraints,
    const OctagonConfig& config)
{
    OctagonState state = top(environment, config);
    state.assumeAll(constraints);
    return state;
}

OctagonState::OctagonState(const OctagonState& other)
    : NumericalState(other), environment_(other.environment_),
      impl_(std::make_unique<Impl>(*other.impl_))
{
}

OctagonState::OctagonState(OctagonState&& other) noexcept = default;

OctagonState& OctagonState::operator=(const OctagonState& other)
{
    if (this == &other)
        return *this;
    NumericalState::operator=(other);
    environment_ = other.environment_;
    impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

OctagonState& OctagonState::operator=(OctagonState&& other) noexcept = default;

OctagonState::~OctagonState() = default;

std::unique_ptr<AbstractState> OctagonState::clone() const
{
    return std::make_unique<OctagonState>(*this);
}

const char* OctagonState::name() const
{
    return impl_->name();
}

DomainCapabilities OctagonState::capabilities() const
{
    return impl_->capabilities();
}

void OctagonState::report(OperationKind operation,
                          ApproximationKind approximation,
                          std::string reason, bool best) const
{
    recordOperation(operation, approximation, best, reason);
    DiagnosticSink* sink = diagnosticSink();
    if (sink && approximation != ApproximationKind::Exact)
        sink->report({operation, approximation, std::move(reason)});
}

void OctagonState::assign(Variable target,
                          const LinearExpression& expression)
{
    const ApproximationKind approximation = assignState(target, expression);
    report(OperationKind::Assignment, approximation,
           approximation == ApproximationKind::UnsupportedFallback
               ? std::string(name()) +
                     " forgot a target assigned an unsupported linear expression"
               : std::string(name()) +
                     " approximated a linear assignment",
           approximation == ApproximationKind::Exact);
}

void OctagonState::assign(Variable target, const TreeExpression& expression)
{
    if (const auto linear = expression.asLinear())
    {
        assign(target, *linear);
        return;
    }
    const Interval value = evaluateTreeExpression(expression);
    assignInterval(target, value);
    report(OperationKind::Assignment,
           ApproximationKind::SoundOverApproximation,
           std::string(name()) +
               " interval-linearized a nonlinear or finite IEEE assignment",
           false);
}

void OctagonState::substitute(Variable target,
                              const LinearExpression& expression)
{
    substituteParallel({{target, expression}});
}

void OctagonState::substituteParallel(
    const LinearAssignmentList& assignments)
{
    std::map<Variable, LinearExpression> replacements;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!environment_.contains(assignment.target))
            throw std::invalid_argument(
                "substitution target is not in environment");
        if (!replacements.emplace(assignment.target, assignment.expression)
                 .second)
            throw std::invalid_argument(
                "parallel substitution contains a duplicate target");
        for (const auto& [variable, coefficient] :
             assignment.expression.terms())
        {
            (void)coefficient;
            if (!environment_.contains(variable))
                throw std::invalid_argument(
                    "substitution expression uses an unknown variable");
        }
    }
    recordOperation(OperationKind::Substitution, ApproximationKind::Exact,
                    true);
    if (assignments.empty() || isBottom())
        return;

    LinearConstraintSet preimage;
    for (const LinearConstraint& constraint : toConstraints())
        preimage.emplace_back(
            constraint.expression().substituted(replacements),
            constraint.kind());
    *this = fromConstraints(environment_, preimage, config());
}

void OctagonState::assume(const LinearConstraint& constraint)
{
    const ApproximationKind approximation = assumeState(constraint);
    report(OperationKind::Assumption, approximation,
           std::string(name()) +
               " ignored or approximated an unsupported constraint",
           approximation == ApproximationKind::Exact);
}

void OctagonState::assume(const TreeConstraint& constraint)
{
    if (const auto linear = constraint.expression().asLinear())
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    const LinearConstraintSet consequences =
        treeConstraintConsequences(constraint);
    assumeAll(consequences);
    report(OperationKind::Assumption,
           ApproximationKind::SoundOverApproximation,
           consequences.empty()
               ? std::string(name()) +
                     " found no affine consequence for a nonlinear or finite IEEE guard"
               : std::string(name()) +
                     " reduced a nonlinear guard to sound affine consequences",
           false);
}

void OctagonState::forget(Variable variable)
{
    forgetState(variable);
    recordOperation(OperationKind::Forget, ApproximationKind::Exact, true);
}

void OctagonState::assignInterval(Variable target, const Interval& value)
{
    impl_->assignIntervalCurrent(environment_, target, value);
}

void OctagonState::projectLowerBounds()
{
    projectLowerBoundsState();
}

void OctagonState::changeEnvironment(const VariableEnvironment& environment,
                                     bool initializeNewVariablesToZero)
{
    if (environment_ == environment)
    {
        recordOperation(OperationKind::EnvironmentChange,
                        ApproximationKind::Exact, true);
        return;
    }
    const VariableEnvironment oldEnvironment = environment_;
    changeEnvironmentState(oldEnvironment, environment, initializeNewVariablesToZero);
    environment_ = environment;
    recordOperation(OperationKind::EnvironmentChange,
                    ApproximationKind::Exact, true);
}

void OctagonState::expand(
    Variable source, const std::vector<VariableDeclaration>& copies)
{
    if (!environment_.contains(source))
        throw std::invalid_argument("expanded variable is not in environment");
    std::set<Variable> seen;
    for (const VariableDeclaration& copy : copies)
    {
        if (environment_.contains(copy.variable) ||
            !seen.insert(copy.variable).second)
            throw std::invalid_argument(
                "expanded variables must be new and unique");
        if (copy.type != environment_.typeOf(source))
            throw std::invalid_argument(
                "expanded variables must have the source numeric type");
    }
    if (copies.empty())
    {
        recordOperation(OperationKind::Expand, ApproximationKind::Exact, true);
        return;
    }
    const Interval sourceValue = bound(source);
    const LinearConstraintSet original = toConstraints();
    const bool relational =
        environment_.typeOf(source).kind != NumericKind::IEEEFloat;
    changeEnvironment(environment_.add(copies));
    for (const VariableDeclaration& copy : copies)
    {
        if (relational)
        {
            std::map<Variable, LinearExpression> replacement{
                {source, LinearExpression(copy.variable)}};
            LinearConstraintSet duplicated;
            duplicated.reserve(original.size());
            for (const LinearConstraint& constraint : original)
            {
                duplicated.emplace_back(
                    constraint.expression().substituted(replacement),
                    constraint.kind());
            }
            assumeAll(duplicated);
        }
        else
            assignInterval(copy.variable, sourceValue);
    }
    recordOperation(OperationKind::Expand,
                    relational ? ApproximationKind::Exact
                               : ApproximationKind::SoundOverApproximation,
                    relational,
                    relational ? "" : "IEEE expand retained finite bounds");
}

void OctagonState::fold(Variable target,
                        const std::vector<Variable>& folded)
{
    if (!environment_.contains(target))
        throw std::invalid_argument("fold target is not in environment");
    std::set<Variable> seen;
    std::vector<Variable> sources{target};
    for (Variable variable : folded)
    {
        if (variable == target || !environment_.contains(variable) ||
            !seen.insert(variable).second)
            throw std::invalid_argument(
                "folded variables must be distinct non-target dimensions");
        if (environment_.typeOf(variable) != environment_.typeOf(target))
            throw std::invalid_argument(
                "folded variables must have the target numeric type");
        sources.push_back(variable);
    }
    if (folded.empty())
    {
        recordOperation(OperationKind::Fold, ApproximationKind::Exact, true);
        return;
    }
    const bool relational =
        environment_.typeOf(target).kind != NumericKind::IEEEFloat;
    OctagonState result = bottom(environment_, config());
    for (Variable source : sources)
    {
        OctagonState branch = *this;
        if (source != target)
        {
            if (relational)
                branch.assign(target, LinearExpression(source));
            else
                branch.assignInterval(target, bound(source));
        }
        result = result.join(branch);
    }
    result.changeEnvironment(environment_.remove(folded));
    *this = std::move(result);
    recordOperation(OperationKind::Fold,
                    relational ? ApproximationKind::Exact
                               : ApproximationKind::SoundOverApproximation,
                    relational,
                    relational ? "" : "IEEE fold retained finite hull bounds");
}

CheckResult OctagonState::entails(const LinearConstraint& constraint) const
{
    if (constraint.kind() == ConstraintKind::Equal)
    {
        const CheckResult le = entails(LinearConstraint(
            constraint.expression(), ConstraintKind::LessEqual));
        const CheckResult ge = entails(LinearConstraint(
            constraint.expression(), ConstraintKind::GreaterEqual));
        if (le == CheckResult::True && ge == CheckResult::True)
            return CheckResult::True;
        if (le == CheckResult::False || ge == CheckResult::False)
            return CheckResult::False;
        return CheckResult::Unknown;
    }

    if (constraint.kind() == ConstraintKind::NotEqual)
    {
        OctagonState witness(*this);
        witness.assume(LinearConstraint(constraint.expression(),
                                        ConstraintKind::Equal));
        return witness.isBottom() ? CheckResult::True : CheckResult::False;
    }

    ConstraintKind negated;
    switch (constraint.kind())
    {
    case ConstraintKind::LessThan:
        negated = ConstraintKind::GreaterEqual;
        break;
    case ConstraintKind::LessEqual:
        negated = ConstraintKind::GreaterThan;
        break;
    case ConstraintKind::GreaterThan:
        negated = ConstraintKind::LessEqual;
        break;
    case ConstraintKind::GreaterEqual:
        negated = ConstraintKind::LessThan;
        break;
    case ConstraintKind::Equal:
    case ConstraintKind::NotEqual:
        throw std::logic_error("equality entailment was not normalized");
    }
    OctagonState counterexample(*this);
    counterexample.assume(
        LinearConstraint(constraint.expression(), negated));
    return counterexample.isBottom() ? CheckResult::True : CheckResult::False;
}

Interval OctagonState::bound(Variable variable) const
{
    return boundState(variable);
}

Interval OctagonState::bound(const LinearExpression& expression) const
{
    return impl_->boundExpressionCurrent(environment_, expression);
}

IntervalBox OctagonState::toBox() const
{
    IntervalBox box;
    for (const VariableDeclaration& declaration : environment_.variables())
        box.bounds.emplace(declaration.variable, bound(declaration.variable));
    return box;
}

LinearConstraintSet OctagonState::toConstraints() const
{
    return constraintsState();
}

void OctagonState::close()
{
    recordOperation(OperationKind::TopologicalClosure,
                    ApproximationKind::Exact, true);
    if (isBottom())
        return;
    LinearConstraintSet closed;
    for (const LinearConstraint& constraint : toConstraints())
    {
        ConstraintKind kind = constraint.kind();
        if (kind == ConstraintKind::LessThan)
            kind = ConstraintKind::LessEqual;
        else if (kind == ConstraintKind::GreaterThan)
            kind = ConstraintKind::GreaterEqual;
        closed.emplace_back(constraint.expression(), kind);
    }
    *this = fromConstraints(environment_, closed, config());
}

void OctagonState::canonicalize()
{
    impl_->canonicalizeCurrent();
    recordOperation(OperationKind::Canonicalization,
                    ApproximationKind::Exact, true);
}

const OctagonConfig& OctagonState::config() const
{
    return impl_->config();
}

OctagonStorageStats OctagonState::storageStats() const
{
    return impl_->storageStats();
}

OctagonState OctagonState::reconfigured(const OctagonConfig& config) const
{
    OctagonState result(*this);
    result.impl_->reconfigure(config);
    return result;
}

OctagonState OctagonState::join(const OctagonState& other) const
{
    requireCompatible(other);
    OctagonState result(environment(), impl_->joined(*other.impl_));
    result.recordOperation(OperationKind::Join, ApproximationKind::Exact,
                           true);
    return result;
}

OctagonState OctagonState::meet(const OctagonState& other) const
{
    requireCompatible(other);
    OctagonState result(environment(), impl_->met(*other.impl_));
    result.recordOperation(OperationKind::Meet, ApproximationKind::Exact,
                           true);
    return result;
}

OctagonState OctagonState::widen(
    const OctagonState& next, const WideningPolicy& policy) const
{
    requireCompatible(next);
    OctagonState result(environment(), impl_->widened(*next.impl_, policy));
    for (const LinearConstraint& threshold : policy.linearThresholds)
    {
        if (entails(threshold) == CheckResult::True &&
            next.entails(threshold) == CheckResult::True)
            result.assume(threshold);
    }
    result.recordOperation(OperationKind::Widening,
                           ApproximationKind::SoundOverApproximation, true);
    return result;
}

OctagonState OctagonState::narrow(const OctagonState& next) const
{
    requireCompatible(next);
    if (!next.leqState(*this))
        throw std::invalid_argument(
            "narrowing requires next to be included in current");
    OctagonState result(environment(), impl_->narrowed(*next.impl_));
    result.recordOperation(OperationKind::Narrowing,
                           ApproximationKind::Exact, true);
    return result;
}

OctagonState OctagonState::projectedLowerBounds() const
{
    return OctagonState(environment(), impl_->projectedLowerBounds());
}

OctagonState OctagonState::withEnvironment(
    const VariableEnvironment& environment, bool initializeNewVariablesToZero) const
{
    return OctagonState(
        environment,
        impl_->changedEnvironment(this->environment(), environment,
                                  initializeNewVariablesToZero));
}

DiagnosticSink* OctagonState::diagnosticSink() const
{
    return impl_->config().diagnostics.get();
}

const OctagonState& OctagonState::requireOctagon(
    const AbstractState& other) const
{
    const auto* octagon = other.isState<OctagonState>()
                              ? &static_cast<const OctagonState&>(other)
                              : nullptr;
    if (!octagon)
        throw std::invalid_argument("relational state is not an OctagonState");
    return *octagon;
}

bool OctagonState::hasCompatibleDomain(const AbstractState& other) const
{
    const auto* octagon = other.isState<OctagonState>()
                              ? &static_cast<const OctagonState&>(other)
                              : nullptr;
    return octagon && environment_ == octagon->environment_ &&
           config().operationCompatible(octagon->config());
}

ApproximationKind OctagonState::assignState(
    Variable target, const LinearExpression& expression)
{
    return impl_->assignCurrent(environment(), target, expression);
}

ApproximationKind OctagonState::assumeState(
    const LinearConstraint& constraint)
{
    return impl_->assumeCurrent(environment(), constraint);
}

void OctagonState::forgetState(Variable variable)
{
    impl_->forgetCurrent(environment(), variable);
}

void OctagonState::joinState(const AbstractState& other)
{
    impl_->joinCurrent(*requireOctagon(other).impl_);
}

void OctagonState::meetState(const AbstractState& other)
{
    impl_->meetCurrent(*requireOctagon(other).impl_);
}

void OctagonState::widenState(const AbstractState& next)
{
    impl_->widenCurrent(*requireOctagon(next).impl_, {});
}

void OctagonState::narrowState(const AbstractState& next)
{
    impl_->narrowCurrent(*requireOctagon(next).impl_);
}

void OctagonState::projectLowerBoundsState()
{
    impl_->projectLowerBoundsCurrent();
}

void OctagonState::changeEnvironmentState(
    const VariableEnvironment& oldEnvironment, const VariableEnvironment& newEnvironment,
    bool initializeNewVariablesToZero)
{
    impl_->changeEnvironmentCurrent(oldEnvironment, newEnvironment,
                                    initializeNewVariablesToZero);
}

bool OctagonState::isBottomState() const
{
    return impl_->isBottomCurrent();
}

bool OctagonState::isTopState() const
{
    return impl_->isTopCurrent();
}

bool OctagonState::leqState(const AbstractState& other) const
{
    return impl_->leqCurrent(*requireOctagon(other).impl_);
}

Interval OctagonState::boundState(Variable variable) const
{
    return impl_->boundCurrent(environment(), variable);
}

LinearConstraintSet OctagonState::constraintsState() const
{
    return impl_->constraintsCurrent(environment());
}

std::string OctagonState::stateToString() const
{
    return impl_->toStringCurrent(environment());
}

} // namespace SVF::AbstractDomain
