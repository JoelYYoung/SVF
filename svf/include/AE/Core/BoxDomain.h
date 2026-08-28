//===- BoxDomain.h -- Exact-rational interval box state --------*- C++ -*-===//

#ifndef SVF_AE_BOX_DOMAIN_H
#define SVF_AE_BOX_DOMAIN_H

#include "AE/Core/AbstractState.h"
#include "AE/Core/NumericalDomain.h"

#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace SVF::AbstractDomain
{

enum class BoxStorageRole : std::uint8_t
{
    General,
    Scalar,
    Flow,
    Count
};

enum class BoxStorageOperation : std::uint8_t
{
    CreateTop,
    CreateBottom,
    ImportBox,
    ImportConstraints,
    BoundVariable,
    BoundExpression,
    AssignLinear,
    AssignTree,
    AssignParallel,
    Substitute,
    SubstituteParallel,
    AssumeLinear,
    AssumeTree,
    Forget,
    EnvironmentChange,
    EnvironmentNoop,
    Expand,
    Fold,
    Entails,
    ExportBox,
    ExportConstraints,
    Close,
    Canonicalize,
    Join,
    Meet,
    Widen,
    Narrow,
    Leq,
    Clone,
    PageLookupHit,
    PageLookupMiss,
    SlotLookupHit,
    SlotLookupMiss,
    PageCreate,
    PageDetach,
    DirectoryDetach,
    PageErase,
    Count
};

struct BoxStorageMetrics
{
    std::size_t environmentDimensions = 0;
    std::size_t materializedBounds = 0;
    std::size_t pageReferences = 0;
    std::size_t allocatedSlots = 0;
};

/// Opt-in, single-threaded production telemetry for representation decisions.
/// It is observational: compatibility and abstract semantics ignore it.
class BoxStorageTelemetry
{
public:
    bool record(BoxStorageRole role, BoxStorageOperation operation,
                bool shapeEligible = true);
    void recordShape(BoxStorageRole role, const BoxStorageMetrics& metrics);
    void recordResidentShape(BoxStorageRole role,
                             const BoxStorageMetrics& metrics);
    void recordPageDetach(BoxStorageRole role, std::size_t sharingFanout);
    void recordDirectoryShift(BoxStorageRole role, std::size_t entries);
    void report(std::ostream& output) const;

private:
    static constexpr std::size_t RoleCount =
        static_cast<std::size_t>(BoxStorageRole::Count);
    static constexpr std::size_t OperationCount =
        static_cast<std::size_t>(BoxStorageOperation::Count);
    static constexpr std::size_t HistogramBins = 18;

    struct ShapeAggregate
    {
        std::uint64_t samples = 0;
        std::uint64_t environmentDimensions = 0;
        std::uint64_t materializedBounds = 0;
        std::uint64_t pageReferences = 0;
        std::uint64_t allocatedSlots = 0;
        std::size_t maxEnvironmentDimensions = 0;
        std::size_t maxMaterializedBounds = 0;
        std::size_t maxPageReferences = 0;
        std::array<std::uint64_t, HistogramBins> environmentHistogram{};
        std::array<std::uint64_t, HistogramBins> materializedHistogram{};
        std::array<std::uint64_t, HistogramBins> pageHistogram{};
    };

    static void accumulateShape(ShapeAggregate& shape,
                                const BoxStorageMetrics& metrics);

    std::array<std::array<std::uint64_t, OperationCount>, RoleCount>
        operations_{};
    std::array<std::uint64_t, RoleCount> events_{};
    std::array<std::uint64_t, RoleCount> detachedSharingFanout_{};
    std::array<std::size_t, RoleCount> maxDetachedSharingFanout_{};
    std::array<std::uint64_t, RoleCount> directoryEntriesShifted_{};
    std::array<ShapeAggregate, RoleCount> shapes_{};
    std::array<ShapeAggregate, RoleCount> residentShapes_{};
};

struct BoxConfig
{
    bool integerTightening = true;
    bool directoryCOW = false;
    bool hashDirectoryCOW = false;
    std::shared_ptr<DiagnosticSink> diagnostics;
    std::shared_ptr<BoxStorageTelemetry> storageTelemetry;
    BoxStorageRole storageRole = BoxStorageRole::General;

    bool operationCompatible(const BoxConfig& other) const
    {
        return integerTightening == other.integerTightening;
    }
};

/// Non-relational numerical state with one exact-rational interval per
/// environment dimension.
class BoxState final : public NumericalState
{
public:
    using NumericalState::assignParallel;
    using NumericalState::bound;
    using NumericalState::substitute;
    using NumericalState::substituteParallel;

    static BoxState top(const VariableEnvironment& environment,
                        const BoxConfig& config = {});
    static BoxState bottom(const VariableEnvironment& environment,
                           const BoxConfig& config = {});
    static BoxState fromBox(const VariableEnvironment& environment,
                            const IntervalBox& box,
                            const BoxConfig& config = {});
    static BoxState fromConstraints(const VariableEnvironment& environment,
        const LinearConstraintSet& constraints,
        const BoxConfig& config = {});

    BoxState(const BoxState& other);
    BoxState(BoxState&& other) noexcept = default;
    BoxState& operator=(const BoxState& other) = default;
    BoxState& operator=(BoxState&& other) noexcept = default;

    std::unique_ptr<AbstractState> clone() const override;
    const char* name() const override;
    DomainCapabilities capabilities() const override;

    const VariableEnvironment& environment() const override
    {
        return environment_;
    }
    const BoxConfig& config() const
    {
        return config_;
    }

    void assign(Variable target, const LinearExpression& expression) override;
    void assign(Variable target, const TreeExpression& expression) override;
    void assignParallel(const LinearAssignmentList& assignments) override;
    void substitute(Variable target,
                    const LinearExpression& expression) override;
    void substituteParallel(const LinearAssignmentList& assignments) override;
    void assume(const LinearConstraint& constraint) override;
    void assume(const TreeConstraint& constraint) override;
    void forget(Variable variable) override;
    void changeEnvironment(const VariableEnvironment& environment,
                           bool initializeNewVariablesToZero = false) override;
    void expand(Variable source,
                const std::vector<VariableDeclaration>& copies) override;
    void fold(Variable target, const std::vector<Variable>& folded) override;

    CheckResult entails(const LinearConstraint& constraint) const override;
    Interval bound(Variable variable) const override;
    Interval bound(const LinearExpression& expression) const override;
    IntervalBox toBox() const override;
    LinearConstraintSet toConstraints() const override;
    void close() override;
    void canonicalize() override;

    BoxStorageMetrics storageMetrics() const;
    void sampleResidentStorage() const;

    BoxState join(const BoxState& other) const;
    BoxState meet(const BoxState& other) const;
    BoxState widen(const BoxState& next,
                        const WideningPolicy& policy = {}) const;
    BoxState narrow(const BoxState& next) const;

private:
    static constexpr std::size_t BoundsPerPage = 64;

    struct BoundPage
    {
        std::array<std::optional<Interval>, BoundsPerPage> bounds;
    };

    struct BoundPageEntry
    {
        std::size_t index;
        std::shared_ptr<BoundPage> page;
    };

    using BoundPageDirectory = std::vector<BoundPageEntry>;
    using BoundPageHashDirectory =
        std::unordered_map<std::size_t, std::shared_ptr<BoundPage>>;

    BoxState(VariableEnvironment environment, BoxConfig config, bool bottom);

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<BoxState>();
    }
    bool hasCompatibleDomain(const AbstractState& other) const override;
    void joinState(const AbstractState& other) override;
    void meetState(const AbstractState& other) override;
    void widenState(const AbstractState& next) override;
    void narrowState(const AbstractState& next) override;
    bool isBottomState() const override;
    bool isTopState() const override;
    bool leqState(const AbstractState& other) const override;
    std::string stateToString() const override;

    const BoxState& requireBox(const AbstractState& other) const;
    const Interval& boundAt(Dimension dimension) const;
    const BoundPageDirectory& pageDirectory() const;
    BoundPageDirectory& writablePageDirectory();
    BoundPageHashDirectory& writableHashPageDirectory();
    BoundPage& writablePage(std::size_t pageIndex);
    void eraseBound(Dimension dimension);
    static bool pageIsEmpty(const BoundPage& page);
    std::vector<Dimension> boundedDimensions() const;
    void makeBottom();
    void canonicalize(Dimension dimension);
    void setBound(Dimension dimension, Interval interval);
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason, bool best = true) const;
    void profileStorage(BoxStorageOperation operation) const;
    void recordStorage(BoxStorageOperation operation) const;

    VariableEnvironment environment_;
    BoxConfig config_;
    /// Missing pages and empty slots denote top. Active pages are kept sorted,
    /// shared by state copies, and detached only when one of their bounds
    /// changes.
    BoundPageDirectory boundPages_;
    std::shared_ptr<BoundPageDirectory> cowBoundPages_;
    std::shared_ptr<BoundPageHashDirectory> cowHashBoundPages_;
    bool bottom_ = false;
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_BOX_DOMAIN_H
