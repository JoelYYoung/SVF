//===- BoxDomain.cpp -- Exact-rational interval box state ----------------===//

#include "AE/Core/BoxDomain.h"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SVF::AbstractDomain
{

namespace
{

constexpr std::size_t telemetryRoleIndex(BoxStorageRole role)
{
    return static_cast<std::size_t>(role);
}

constexpr std::size_t telemetryOperationIndex(BoxStorageOperation operation)
{
    return static_cast<std::size_t>(operation);
}

const char* telemetryRoleName(BoxStorageRole role)
{
    switch (role)
    {
    case BoxStorageRole::General:
        return "general";
    case BoxStorageRole::Scalar:
        return "scalar";
    case BoxStorageRole::Flow:
        return "flow";
    case BoxStorageRole::Count:
        break;
    }
    return "unknown";
}

const char* telemetryOperationName(BoxStorageOperation operation)
{
    switch (operation)
    {
    case BoxStorageOperation::CreateTop: return "create-top";
    case BoxStorageOperation::CreateBottom: return "create-bottom";
    case BoxStorageOperation::ImportBox: return "import-box";
    case BoxStorageOperation::ImportConstraints: return "import-constraints";
    case BoxStorageOperation::BoundVariable: return "bound-variable";
    case BoxStorageOperation::BoundExpression: return "bound-expression";
    case BoxStorageOperation::AssignLinear: return "assign-linear";
    case BoxStorageOperation::AssignTree: return "assign-tree";
    case BoxStorageOperation::AssignParallel: return "assign-parallel";
    case BoxStorageOperation::Substitute: return "substitute";
    case BoxStorageOperation::SubstituteParallel: return "substitute-parallel";
    case BoxStorageOperation::AssumeLinear: return "assume-linear";
    case BoxStorageOperation::AssumeTree: return "assume-tree";
    case BoxStorageOperation::Forget: return "forget";
    case BoxStorageOperation::EnvironmentChange: return "environment-change";
    case BoxStorageOperation::EnvironmentNoop: return "environment-noop";
    case BoxStorageOperation::Expand: return "expand";
    case BoxStorageOperation::Fold: return "fold";
    case BoxStorageOperation::Entails: return "entails";
    case BoxStorageOperation::ExportBox: return "export-box";
    case BoxStorageOperation::ExportConstraints: return "export-constraints";
    case BoxStorageOperation::Close: return "close";
    case BoxStorageOperation::Canonicalize: return "canonicalize";
    case BoxStorageOperation::Join: return "join";
    case BoxStorageOperation::Meet: return "meet";
    case BoxStorageOperation::Widen: return "widen";
    case BoxStorageOperation::Narrow: return "narrow";
    case BoxStorageOperation::Leq: return "leq";
    case BoxStorageOperation::Clone: return "clone";
    case BoxStorageOperation::PageLookupHit: return "page-lookup-hit";
    case BoxStorageOperation::PageLookupMiss: return "page-lookup-miss";
    case BoxStorageOperation::SlotLookupHit: return "slot-lookup-hit";
    case BoxStorageOperation::SlotLookupMiss: return "slot-lookup-miss";
    case BoxStorageOperation::PageCreate: return "page-create";
    case BoxStorageOperation::PageDetach: return "page-detach";
    case BoxStorageOperation::DirectoryDetach: return "directory-detach";
    case BoxStorageOperation::PageErase: return "page-erase";
    case BoxStorageOperation::Count: break;
    }
    return "unknown";
}

std::size_t logarithmicBucket(std::size_t value, std::size_t bins)
{
    if (value == 0)
        return 0;
    std::size_t bucket = 1;
    while (value > 1 && bucket + 1 < bins)
    {
        value >>= 1;
        ++bucket;
    }
    return bucket;
}

int compareLower(const Bound& lhs, const Bound& rhs)
{
    if (lhs.kind() != rhs.kind())
        return static_cast<int>(lhs.kind()) < static_cast<int>(rhs.kind()) ? -1
                   : 1;
    if (!lhs.isFinite())
        return 0;
    if (lhs.value() < rhs.value())
        return -1;
    if (rhs.value() < lhs.value())
        return 1;
    if (lhs.isStrict() == rhs.isStrict())
        return 0;
    return lhs.isStrict() ? 1 : -1;
}

Bound minLower(const Bound& lhs, const Bound& rhs)
{
    return compareLower(lhs, rhs) <= 0 ? lhs : rhs;
}

Bound maxLower(const Bound& lhs, const Bound& rhs)
{
    return compareLower(lhs, rhs) >= 0 ? lhs : rhs;
}

Bound scaleBound(const Bound& bound, const Rational& coefficient)
{
    if (coefficient.isZero())
        return Bound::finite(Rational());
    if (bound.isMinusInfinity())
        return coefficient.sign() > 0 ? Bound::minusInfinity()
                                      : Bound::plusInfinity();
    if (bound.isPlusInfinity())
        return coefficient.sign() > 0 ? Bound::plusInfinity()
                                      : Bound::minusInfinity();
    return Bound::finite(bound.value() * coefficient, bound.isStrict());
}

Interval scaleInterval(const Interval& interval, const Rational& coefficient)
{
    if (coefficient.isZero())
        return Interval::singleton(Rational());
    if (coefficient.sign() > 0)
        return Interval(scaleBound(interval.lower(), coefficient),
                        scaleBound(interval.upper(), coefficient));
    return Interval(scaleBound(interval.upper(), coefficient),
                    scaleBound(interval.lower(), coefficient));
}

Interval addIntervals(const Interval& lhs, const Interval& rhs)
{
    return Interval(Bound::add(lhs.lower(), rhs.lower()),
                    Bound::add(lhs.upper(), rhs.upper()));
}

Interval joinIntervals(const Interval& lhs, const Interval& rhs)
{
    return Interval(minLower(lhs.lower(), rhs.lower()),
                    Bound::max(lhs.upper(), rhs.upper()));
}

Interval meetIntervals(const Interval& lhs, const Interval& rhs)
{
    return Interval(maxLower(lhs.lower(), rhs.lower()),
                    Bound::min(lhs.upper(), rhs.upper()));
}

bool intervalIncluded(const Interval& lhs, const Interval& rhs)
{
    if (lhs.isBottom())
        return true;
    if (rhs.isBottom())
        return false;
    return compareLower(lhs.lower(), rhs.lower()) >= 0 &&
           Bound::compare(lhs.upper(), rhs.upper()) <= 0;
}

Interval evaluate(const BoxState& state, const LinearExpression& expression,
                  std::optional<Variable> excluded = std::nullopt)
{
    Interval result = Interval::singleton(expression.constant());
    for (const auto& [variable, coefficient] : expression.terms())
    {
        if (excluded && variable == *excluded)
            continue;
        result = addIntervals(
            result, scaleInterval(state.bound(variable), coefficient));
    }
    return result;
}

Bound integerLower(Bound bound)
{
    if (!bound.isFinite())
        return bound;
    const Rational value = bound.isStrict()
                               ? bound.value().floor() + Rational(1)
                               : bound.value().ceil();
    return Bound::finite(value);
}

Bound integerUpper(Bound bound)
{
    if (!bound.isFinite())
        return bound;
    const Rational value = bound.isStrict() ? bound.value().ceil() - Rational(1)
                                            : bound.value().floor();
    return Bound::finite(value);
}

LinearConstraint normalizedLessEqual(const LinearConstraint& constraint,
                                     bool& strict)
{
    strict = constraint.kind() == ConstraintKind::LessThan ||
             constraint.kind() == ConstraintKind::GreaterThan;
    if (constraint.kind() == ConstraintKind::GreaterEqual ||
            constraint.kind() == ConstraintKind::GreaterThan)
        return LinearConstraint(-constraint.expression(),
                                strict ? ConstraintKind::LessThan
                                       : ConstraintKind::LessEqual);
    return LinearConstraint(constraint.expression(),
                            strict ? ConstraintKind::LessThan
                                   : ConstraintKind::LessEqual);
}

} // namespace

bool BoxStorageTelemetry::record(BoxStorageRole role,
                                 BoxStorageOperation operation,
                                 bool shapeEligible)
{
    ++operations_[telemetryRoleIndex(role)]
                 [telemetryOperationIndex(operation)];
    if (!shapeEligible)
        return false;
    const std::uint64_t event = ++events_[telemetryRoleIndex(role)];
    return event == 1 || event % 512 == 0;
}

void BoxStorageTelemetry::accumulateShape(ShapeAggregate& shape,
                                          const BoxStorageMetrics& metrics)
{
    ++shape.samples;
    shape.environmentDimensions += metrics.environmentDimensions;
    shape.materializedBounds += metrics.materializedBounds;
    shape.pageReferences += metrics.pageReferences;
    shape.allocatedSlots += metrics.allocatedSlots;
    shape.maxEnvironmentDimensions =
        std::max(shape.maxEnvironmentDimensions,
                 metrics.environmentDimensions);
    shape.maxMaterializedBounds =
        std::max(shape.maxMaterializedBounds, metrics.materializedBounds);
    shape.maxPageReferences =
        std::max(shape.maxPageReferences, metrics.pageReferences);
    ++shape.environmentHistogram[logarithmicBucket(
        metrics.environmentDimensions, HistogramBins)];
    ++shape.materializedHistogram[logarithmicBucket(
        metrics.materializedBounds, HistogramBins)];
    ++shape.pageHistogram[logarithmicBucket(metrics.pageReferences,
                                            HistogramBins)];
}

void BoxStorageTelemetry::recordShape(BoxStorageRole role,
                                      const BoxStorageMetrics& metrics)
{
    accumulateShape(shapes_[telemetryRoleIndex(role)], metrics);
}

void BoxStorageTelemetry::recordResidentShape(
    BoxStorageRole role, const BoxStorageMetrics& metrics)
{
    accumulateShape(residentShapes_[telemetryRoleIndex(role)], metrics);
}

void BoxStorageTelemetry::recordPageDetach(BoxStorageRole role,
                                           std::size_t sharingFanout)
{
    const std::size_t index = telemetryRoleIndex(role);
    detachedSharingFanout_[index] += sharingFanout;
    maxDetachedSharingFanout_[index] =
        std::max(maxDetachedSharingFanout_[index], sharingFanout);
}

void BoxStorageTelemetry::recordDirectoryShift(BoxStorageRole role,
                                               std::size_t entries)
{
    directoryEntriesShifted_[telemetryRoleIndex(role)] += entries;
}

void BoxStorageTelemetry::report(std::ostream& output) const
{
    for (std::size_t role = 0; role < RoleCount; ++role)
    {
        const auto storageRole = static_cast<BoxStorageRole>(role);
        for (std::size_t operation = 0; operation < OperationCount;
             ++operation)
        {
            const std::uint64_t calls = operations_[role][operation];
            if (calls == 0)
                continue;
            output << "BOX_STORAGE_OP role=" << telemetryRoleName(storageRole)
                   << " operation="
                   << telemetryOperationName(
                          static_cast<BoxStorageOperation>(operation))
                   << " calls=" << calls << '\n';
        }
        auto reportShape = [&](const char* kind,
                               const ShapeAggregate& shape)
        {
            const double utilization =
                shape.allocatedSlots == 0
                    ? 0.0
                    : static_cast<double>(shape.materializedBounds) /
                          static_cast<double>(shape.allocatedSlots);
            output << "BOX_STORAGE_SHAPE role="
                   << telemetryRoleName(storageRole)
                   << " kind=" << kind
                   << " samples=" << shape.samples
                   << " environment_sum=" << shape.environmentDimensions
                   << " materialized_sum=" << shape.materializedBounds
                   << " page_references_sum=" << shape.pageReferences
                   << " allocated_slots_sum=" << shape.allocatedSlots
                   << " utilization=" << std::fixed << std::setprecision(6)
                   << utilization
                   << " max_environment=" << shape.maxEnvironmentDimensions
                   << " max_materialized=" << shape.maxMaterializedBounds
                   << " max_pages=" << shape.maxPageReferences
                   << " detach_fanout_sum=" << detachedSharingFanout_[role]
                   << " max_detach_fanout=" << maxDetachedSharingFanout_[role]
                   << " directory_entries_shifted="
                   << directoryEntriesShifted_[role] << '\n';
            auto reportHistogram = [&](const char* name,
                                       const auto& histogram)
            {
                for (std::size_t bucket = 0; bucket < histogram.size();
                     ++bucket)
                {
                    if (histogram[bucket] == 0)
                        continue;
                    output << "BOX_STORAGE_HIST role="
                           << telemetryRoleName(storageRole)
                           << " kind=" << kind << " metric=" << name
                           << " log2_bucket=" << bucket
                           << " samples=" << histogram[bucket] << '\n';
                }
            };
            reportHistogram("environment", shape.environmentHistogram);
            reportHistogram("materialized", shape.materializedHistogram);
            reportHistogram("pages", shape.pageHistogram);
        };
        reportShape("operation-sample", shapes_[role]);
        reportShape("resident", residentShapes_[role]);
    }
}

BoxState::BoxState(VariableEnvironment environment, BoxConfig config,
                   bool bottom)
    : environment_(std::move(environment)), config_(std::move(config)),
      bottom_(bottom)
{
    if (config_.directoryCOW && config_.hashDirectoryCOW)
        throw std::invalid_argument(
            "Box vector-directory COW and hash-directory COW are exclusive");
    if (config_.hashDirectoryCOW)
        cowHashBoundPages_ = std::make_shared<BoundPageHashDirectory>();
    else if (config_.directoryCOW)
        cowBoundPages_ = std::make_shared<BoundPageDirectory>();
}

BoxState::BoxState(const BoxState& other)
    : NumericalState(other), environment_(other.environment_),
      config_(other.config_), boundPages_(other.boundPages_),
      cowBoundPages_(other.cowBoundPages_),
      cowHashBoundPages_(other.cowHashBoundPages_), bottom_(other.bottom_)
{
    profileStorage(BoxStorageOperation::Clone);
}

BoxState BoxState::top(const VariableEnvironment& environment,
                       const BoxConfig& config)
{
    BoxState result(environment, config, false);
    result.profileStorage(BoxStorageOperation::CreateTop);
    return result;
}

BoxState BoxState::bottom(const VariableEnvironment& environment,
                          const BoxConfig& config)
{
    BoxState result(environment, config, true);
    result.profileStorage(BoxStorageOperation::CreateBottom);
    return result;
}

BoxState BoxState::fromBox(const VariableEnvironment& environment,
                           const IntervalBox& box, const BoxConfig& config)
{
    BoxState result = top(environment, config);
    result.profileStorage(BoxStorageOperation::ImportBox);
    for (const auto& [variable, interval] : box.bounds)
    {
        if (!environment.contains(variable))
            throw std::invalid_argument("box contains an unknown variable");
        result.setBound(environment.dimensionOf(variable), interval);
    }
    return result;
}

BoxState BoxState::fromConstraints(const VariableEnvironment& environment,
                                   const LinearConstraintSet& constraints,
                                   const BoxConfig& config)
{
    BoxState result = top(environment, config);
    result.profileStorage(BoxStorageOperation::ImportConstraints);
    result.assumeAll(constraints);
    return result;
}

std::unique_ptr<AbstractState> BoxState::clone() const
{
    return std::make_unique<BoxState>(*this);
}

const char* BoxState::name() const
{
    return "BoxState";
}

DomainCapabilities BoxState::capabilities() const
{
    DomainCapabilities result;
    result.strictInequalities = true;
    result.integerTightening = config_.integerTightening;
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

void BoxState::assign(Variable target, const LinearExpression& expression)
{
    profileStorage(BoxStorageOperation::AssignLinear);
    if (!environment_.contains(target))
        throw std::invalid_argument("assignment target is not in environment");
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)coefficient;
        if (!environment_.contains(variable))
            throw std::invalid_argument(
                "assignment expression uses an unknown variable");
    }
    recordOperation(OperationKind::Assignment, ApproximationKind::Exact, true);
    if (bottom_)
        return;
    setBound(environment_.dimensionOf(target), evaluate(*this, expression));
}

void BoxState::assign(Variable target, const TreeExpression& expression)
{
    profileStorage(BoxStorageOperation::AssignTree);
    const std::optional<LinearExpression> linear = expression.asLinear();
    if (linear)
    {
        assign(target, *linear);
        return;
    }
    const Interval value = evaluateTreeExpression(expression);
    if (!bottom_)
        setBound(environment_.dimensionOf(target), value);
    report(OperationKind::Assignment, ApproximationKind::SoundOverApproximation,
           "nonlinear or finite IEEE assignment was interval-linearized",
           false);
}

void BoxState::assignParallel(const LinearAssignmentList& assignments)
{
    profileStorage(BoxStorageOperation::AssignParallel);
    std::set<Variable> targets;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!environment_.contains(assignment.target))
            throw std::invalid_argument(
                "parallel assignment target is not in environment");
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument(
                "parallel assignment contains a duplicate target");
        for (const auto& [variable, coefficient] :
             assignment.expression.terms())
        {
            (void)coefficient;
            if (!environment_.contains(variable))
                throw std::invalid_argument(
                    "parallel assignment expression uses an unknown variable");
        }
    }
    recordOperation(OperationKind::Assignment, ApproximationKind::Exact, true);
    if (bottom_)
        return;

    std::vector<std::pair<Dimension, Interval>> updates;
    updates.reserve(assignments.size());
    for (const LinearAssignment& assignment : assignments)
        updates.emplace_back(environment_.dimensionOf(assignment.target),
                             evaluate(*this, assignment.expression));
    for (auto& [dimension, value] : updates)
        setBound(dimension, std::move(value));
}

void BoxState::substitute(Variable target, const LinearExpression& expression)
{
    profileStorage(BoxStorageOperation::Substitute);
    substituteParallel({{target, expression}});
}

void BoxState::substituteParallel(const LinearAssignmentList& assignments)
{
    profileStorage(BoxStorageOperation::SubstituteParallel);
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
    if (assignments.empty() || bottom_)
        return;

    LinearConstraintSet preimage;
    for (const LinearConstraint& constraint : toConstraints())
        preimage.emplace_back(constraint.expression().substituted(replacements),
            constraint.kind());
    *this = fromConstraints(environment_, preimage, config_);
}

void BoxState::assume(const LinearConstraint& constraint)
{
    profileStorage(BoxStorageOperation::AssumeLinear);
    recordOperation(OperationKind::Assumption, ApproximationKind::Exact, true);
    if (bottom_)
        return;
    for (const auto& [variable, coefficient] : constraint.expression().terms())
    {
        (void)coefficient;
        if (!environment_.contains(variable))
            throw std::invalid_argument("constraint uses an unknown variable");
    }

    if (constraint.kind() == ConstraintKind::NotEqual)
    {
        const Interval value = evaluate(*this, constraint.expression());
        if (!value.lower().isFinite() || !value.upper().isFinite() ||
                value.lower().value() != Rational() ||
            value.upper().value() != Rational() || value.lower().isStrict() ||
            value.upper().isStrict())
            return;
        makeBottom();
        return;
    }

    if (constraint.kind() == ConstraintKind::Equal)
    {
        assume(LinearConstraint(constraint.expression(),
                                ConstraintKind::LessEqual));
        assume(LinearConstraint(-constraint.expression(),
                                ConstraintKind::LessEqual));
        return;
    }

    bool strict = false;
    const LinearConstraint normalized = normalizedLessEqual(constraint, strict);
    const LinearExpression& expression = normalized.expression();

    // Repeating interval propagation lets bounds inferred for one dimension
    // tighten another without introducing an unbounded worklist.
    for (std::size_t pass = 0; pass <= environment_.size(); ++pass)
    {
        bool changed = false;
        for (const auto& [variable, coefficient] : expression.terms())
        {
            if (coefficient.isZero())
                continue;
            const Interval rest = evaluate(*this, expression, variable);
            if (!rest.lower().isFinite())
                continue;

            const Rational rhs = -rest.lower().value() / coefficient;
            const bool resultStrict = strict || rest.lower().isStrict();
            const Dimension dimension = environment_.dimensionOf(variable);
            Interval next = boundAt(dimension);
            if (coefficient.sign() > 0)
            {
                next = meetIntervals(
                    next, Interval(Bound::minusInfinity(),
                             Bound::finite(rhs, resultStrict)));
            }
            else
            {
                next = meetIntervals(next,
                    Interval(Bound::finite(rhs, resultStrict),
                             Bound::plusInfinity()));
            }
            const Interval previous = boundAt(dimension);
            setBound(dimension, next);
            if (bottom_)
                return;
            changed = changed ||
                      !intervalIncluded(previous, boundAt(dimension)) ||
                      !intervalIncluded(boundAt(dimension), previous);
        }
        if (!changed)
            break;
    }

    const Interval value = evaluate(*this, expression);
    if (value.lower().isFinite())
    {
        const int sign = value.lower().value().sign();
        if (sign > 0 || (sign == 0 && (strict || value.lower().isStrict())))
            makeBottom();
    }
}

void BoxState::assume(const TreeConstraint& constraint)
{
    profileStorage(BoxStorageOperation::AssumeTree);
    const std::optional<LinearExpression> linear =
        constraint.expression().asLinear();
    if (linear)
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    const LinearConstraintSet consequences =
        treeConstraintConsequences(constraint);
    assumeAll(consequences);
    report(OperationKind::Assumption, ApproximationKind::SoundOverApproximation,
           consequences.empty()
               ? "nonlinear or finite IEEE guard had no affine consequence"
               : "nonlinear guard was reduced to sound affine consequences",
           false);
}

void BoxState::forget(Variable variable)
{
    profileStorage(BoxStorageOperation::Forget);
    if (!environment_.contains(variable))
        throw std::invalid_argument("forgotten variable is not in environment");
    if (!bottom_)
        eraseBound(environment_.dimensionOf(variable));
    recordOperation(OperationKind::Forget, ApproximationKind::Exact, true);
}

void BoxState::changeEnvironment(const VariableEnvironment& environment,
                                 bool initializeNewVariablesToZero)
{
    if (environment_ == environment)
    {
        profileStorage(BoxStorageOperation::EnvironmentNoop);
        recordOperation(OperationKind::EnvironmentChange,
                        ApproximationKind::Exact, true);
        return;
    }
    profileStorage(BoxStorageOperation::EnvironmentChange);
    for (const VariableDeclaration& declaration : environment.variables())
    {
        if (environment_.contains(declaration.variable) &&
                environment_.typeOf(declaration.variable) != declaration.type)
            throw std::invalid_argument(
                "environment change modifies a variable's numeric type");
    }
    BoxState next = BoxState::top(environment, config_);
    if (bottom_)
        next.makeBottom();
    else
    {
        for (Dimension oldDimension : boundedDimensions())
        {
            const Variable variable = environment_.variableOf(oldDimension);
            if (environment.contains(variable))
                next.setBound(environment.dimensionOf(variable),
                              boundAt(oldDimension));
        }
        if (initializeNewVariablesToZero)
        {
            for (const VariableDeclaration& declaration :
                 environment.variables())
            {
                if (!environment_.contains(declaration.variable))
                    next.setBound(environment.dimensionOf(declaration.variable),
                                  Interval::singleton(Rational()));
        }
    }
    }
    environment_ = std::move(next.environment_);
    boundPages_ = std::move(next.boundPages_);
    cowBoundPages_ = std::move(next.cowBoundPages_);
    cowHashBoundPages_ = std::move(next.cowHashBoundPages_);
    bottom_ = next.bottom_;
    recordOperation(OperationKind::EnvironmentChange, ApproximationKind::Exact,
                    true);
}

void BoxState::expand(Variable source,
                      const std::vector<VariableDeclaration>& copies)
{
    profileStorage(BoxStorageOperation::Expand);
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
    changeEnvironment(environment_.add(copies));
    for (const VariableDeclaration& copy : copies)
        if (!bottom_)
            setBound(environment_.dimensionOf(copy.variable), sourceValue);
    recordOperation(OperationKind::Expand, ApproximationKind::Exact, true);
}

void BoxState::fold(Variable target, const std::vector<Variable>& folded)
{
    profileStorage(BoxStorageOperation::Fold);
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

    BoxState result = bottom(environment_, config_);
    for (Variable source : sources)
    {
        BoxState branch = *this;
        if (source != target)
            branch.setBound(environment_.dimensionOf(target), bound(source));
        result = result.join(branch);
    }
    result.changeEnvironment(environment_.remove(folded));
    *this = std::move(result);
    recordOperation(OperationKind::Fold, ApproximationKind::Exact, true);
}

CheckResult BoxState::entails(const LinearConstraint& constraint) const
{
    profileStorage(BoxStorageOperation::Entails);
    if (bottom_)
        return CheckResult::True;
    const Interval value = evaluate(*this, constraint.expression());
    const auto upperAtMostZero = [&]() {
        if (!value.upper().isFinite())
            return false;
        return value.upper().value().sign() <= 0;
    };
    const auto upperBelowZero = [&]() {
        return value.upper().isFinite() &&
               (value.upper().value().sign() < 0 ||
                (value.upper().value().isZero() && value.upper().isStrict()));
    };
    const auto lowerAtLeastZero = [&]() {
        return value.lower().isFinite() && value.lower().value().sign() >= 0;
    };
    const auto lowerAboveZero = [&]() {
        return value.lower().isFinite() &&
               (value.lower().value().sign() > 0 ||
                (value.lower().value().isZero() && value.lower().isStrict()));
    };

    switch (constraint.kind())
    {
    case ConstraintKind::LessEqual:
        return upperAtMostZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::LessThan:
        return upperBelowZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::GreaterEqual:
        return lowerAtLeastZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::GreaterThan:
        return lowerAboveZero() ? CheckResult::True : CheckResult::Unknown;
    case ConstraintKind::Equal:
        return upperAtMostZero() && lowerAtLeastZero() ? CheckResult::True
                                                       : CheckResult::Unknown;
    case ConstraintKind::NotEqual:
        return upperBelowZero() || lowerAboveZero() ? CheckResult::True
                                                    : CheckResult::Unknown;
    }
    return CheckResult::Unknown;
}

Interval BoxState::bound(Variable variable) const
{
    profileStorage(BoxStorageOperation::BoundVariable);
    if (!environment_.contains(variable))
        throw std::invalid_argument("bounded variable is not in environment");
    if (bottom_)
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    return boundAt(environment_.dimensionOf(variable));
}

Interval BoxState::bound(const LinearExpression& expression) const
{
    profileStorage(BoxStorageOperation::BoundExpression);
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)coefficient;
        if (!environment_.contains(variable))
            throw std::invalid_argument(
                "bounded expression uses an unknown variable");
    }
    if (bottom_)
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    return evaluate(*this, expression);
}

IntervalBox BoxState::toBox() const
{
    profileStorage(BoxStorageOperation::ExportBox);
    IntervalBox result;
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
        result.bounds.emplace(environment_.variableOf(dimension),
                              bottom_
                                  ? bound(environment_.variableOf(dimension))
                                  : boundAt(dimension));
    return result;
}

LinearConstraintSet BoxState::toConstraints() const
{
    profileStorage(BoxStorageOperation::ExportConstraints);
    LinearConstraintSet result;
    if (bottom_)
    {
        result.emplace_back(LinearExpression(Rational(1)),
                            ConstraintKind::LessEqual);
        return result;
    }
    for (Dimension dimension : boundedDimensions())
    {
        const Variable variable = environment_.variableOf(dimension);
        const Interval& interval = boundAt(dimension);
        if (interval.lower().isFinite())
        {
            result.emplace_back(LinearExpression(variable) -
                    LinearExpression(interval.lower().value()),
                                interval.lower().isStrict()
                                    ? ConstraintKind::GreaterThan
                                            : ConstraintKind::GreaterEqual);
        }
        if (interval.upper().isFinite())
        {
            result.emplace_back(LinearExpression(variable) -
                    LinearExpression(interval.upper().value()),
                                interval.upper().isStrict()
                                    ? ConstraintKind::LessThan
                                            : ConstraintKind::LessEqual);
        }
    }
    return result;
}

void BoxState::close()
{
    profileStorage(BoxStorageOperation::Close);
    recordOperation(OperationKind::TopologicalClosure, ApproximationKind::Exact,
                    true, "topological closure");
    if (bottom_)
        return;
    for (Dimension dimension : boundedDimensions())
    {
        const Interval& interval = boundAt(dimension);
        const Bound lower = interval.lower().isFinite()
                                ? Bound::finite(interval.lower().value())
                                : interval.lower();
        const Bound upper = interval.upper().isFinite()
                                ? Bound::finite(interval.upper().value())
                                : interval.upper();
        setBound(dimension, Interval(lower, upper));
    }
}

void BoxState::canonicalize()
{
    profileStorage(BoxStorageOperation::Canonicalize);
    for (Dimension dimension : boundedDimensions())
        canonicalize(dimension);
    recordOperation(OperationKind::Canonicalization, ApproximationKind::Exact,
                    true, "canonicalization");
}

BoxState BoxState::join(const BoxState& other) const
{
    BoxState result(*this);
    result.joinState(other);
    result.recordOperation(OperationKind::Join, ApproximationKind::Exact, true);
    return result;
}

BoxState BoxState::meet(const BoxState& other) const
{
    BoxState result(*this);
    result.meetState(other);
    result.recordOperation(OperationKind::Meet, ApproximationKind::Exact, true);
    return result;
}

BoxState BoxState::widen(const BoxState& next,
                              const WideningPolicy& policy) const
{
    profileStorage(BoxStorageOperation::Widen);
    requireBox(next);
    if (bottom_)
    {
        BoxState result(next);
        result.recordOperation(OperationKind::Widening,
                               ApproximationKind::SoundOverApproximation, true);
        return result;
    }
    if (next.bottom_)
    {
        BoxState result(*this);
        result.recordOperation(OperationKind::Widening,
                               ApproximationKind::SoundOverApproximation, true);
        return result;
    }
    BoxState result(*this);
    for (Dimension dimension : boundedDimensions())
    {
        Bound lower = boundAt(dimension).lower();
        Bound upper = boundAt(dimension).upper();
        const Interval& following = next.boundAt(dimension);
        if (compareLower(following.lower(), lower) < 0)
        {
            lower = Bound::minusInfinity();
            if (following.lower().isFinite())
            {
                for (const Rational& threshold : policy.thresholds)
                {
                    if (threshold <= following.lower().value() &&
                        (lower.isMinusInfinity() || lower.value() < threshold))
                        lower = Bound::finite(threshold);
                }
            }
        }
        if (Bound::compare(following.upper(), upper) > 0)
        {
            upper = Bound::plusInfinity();
            if (following.upper().isFinite())
            {
                for (const Rational& threshold : policy.thresholds)
                {
                    if (following.upper().value() <= threshold &&
                        (upper.isPlusInfinity() || threshold < upper.value()))
                        upper = Bound::finite(threshold);
                }
            }
        }
        result.setBound(dimension, Interval(lower, upper));
    }
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

BoxState BoxState::narrow(const BoxState& next) const
{
    profileStorage(BoxStorageOperation::Narrow);
    requireBox(next);
    if (bottom_ || next.bottom_)
    {
        BoxState result = bottom(environment_, config_);
        result.recordOperation(OperationKind::Narrowing,
                               ApproximationKind::Exact, true);
        return result;
    }
    BoxState result(*this);
    for (Dimension dimension : next.boundedDimensions())
    {
        Bound lower = boundAt(dimension).lower();
        Bound upper = boundAt(dimension).upper();
        if (lower.isMinusInfinity())
            lower = next.boundAt(dimension).lower();
        if (upper.isPlusInfinity())
            upper = next.boundAt(dimension).upper();
        result.setBound(dimension, Interval(lower, upper));
    }
    result.recordOperation(OperationKind::Narrowing, ApproximationKind::Exact,
                           true);
    return result;
}

bool BoxState::hasCompatibleDomain(const AbstractState& other) const
{
    const auto* box = other.isState<BoxState>()
                          ? &static_cast<const BoxState&>(other)
                          : nullptr;
    return box && environment_ == box->environment_ &&
           config_.operationCompatible(box->config_);
}

void BoxState::joinState(const AbstractState& other)
{
    profileStorage(BoxStorageOperation::Join);
    const BoxState& box = requireBox(other);
    if (box.bottom_)
        return;
    if (bottom_)
    {
        *this = box;
        return;
    }
    for (Dimension dimension : boundedDimensions())
        setBound(dimension,
                 joinIntervals(boundAt(dimension), box.boundAt(dimension)));
}

void BoxState::meetState(const AbstractState& other)
{
    profileStorage(BoxStorageOperation::Meet);
    const BoxState& box = requireBox(other);
    if (bottom_ || box.bottom_)
    {
        makeBottom();
        return;
    }
    for (Dimension dimension : box.boundedDimensions())
    {
        setBound(dimension,
                 meetIntervals(boundAt(dimension), box.boundAt(dimension)));
        if (bottom_)
            return;
    }
}

void BoxState::widenState(const AbstractState& next)
{
    *this = widen(requireBox(next));
}

void BoxState::narrowState(const AbstractState& next)
{
    *this = narrow(requireBox(next));
}

bool BoxState::isBottomState() const
{
    return bottom_;
}

bool BoxState::isTopState() const
{
    if (bottom_)
        return false;
    if (config_.hashDirectoryCOW)
        return cowHashBoundPages_->empty();
    return pageDirectory().empty();
}

bool BoxState::leqState(const AbstractState& other) const
{
    profileStorage(BoxStorageOperation::Leq);
    const BoxState& box = requireBox(other);
    if (config_.hashDirectoryCOW && box.config_.hashDirectoryCOW &&
        bottom_ == box.bottom_ &&
        cowHashBoundPages_->size() == box.cowHashBoundPages_->size())
    {
        bool equal = true;
        for (const auto& [pageIndex, page] : *cowHashBoundPages_)
        {
            const auto iterator = box.cowHashBoundPages_->find(pageIndex);
            if (iterator == box.cowHashBoundPages_->end() ||
                (page != iterator->second &&
                 page->bounds != iterator->second->bounds))
            {
                equal = false;
                break;
            }
        }
        if (equal)
            return true;
    }
    const BoundPageDirectory& pages = pageDirectory();
    const BoundPageDirectory& boxPages = box.pageDirectory();
    if (!config_.hashDirectoryCOW && !box.config_.hashDirectoryCOW &&
        bottom_ == box.bottom_ && pages.size() == boxPages.size())
    {
        bool equal = true;
        for (std::size_t index = 0; index < pages.size(); ++index)
        {
            if (pages[index].index != boxPages[index].index ||
                (pages[index].page != boxPages[index].page &&
                 pages[index].page->bounds != boxPages[index].page->bounds))
            {
                equal = false;
                break;
            }
        }
        if (equal)
            return true;
    }
    if (bottom_)
        return true;
    if (box.bottom_)
        return false;
    for (Dimension dimension : box.boundedDimensions())
    {
        if (!intervalIncluded(boundAt(dimension), box.boundAt(dimension)))
            return false;
    }
    return true;
}

std::string BoxState::stateToString() const
{
    if (bottom_)
        return "bottom";
    std::ostringstream output;
    output << "{";
    for (Dimension dimension = 0; dimension < environment_.size(); ++dimension)
    {
        if (dimension != 0)
            output << ", ";
        output << environment_.nameOf(environment_.variableOf(dimension)) << "="
               << boundAt(dimension).toString();
    }
    output << "}";
    return output.str();
}

const BoxState& BoxState::requireBox(const AbstractState& other) const
{
    requireCompatible(other);
    return static_cast<const BoxState&>(other);
}

void BoxState::canonicalize(Dimension dimension)
{
    if (bottom_)
        return;
    Interval interval = boundAt(dimension);
    const Variable variable = environment_.variableOf(dimension);
    if (config_.integerTightening &&
            environment_.typeOf(variable).kind == NumericKind::Integer)
    {
        interval = Interval(integerLower(interval.lower()),
                            integerUpper(interval.upper()));
    }
    if (interval.isBottom())
    {
        makeBottom();
        return;
    }
    if (interval.isTop())
        eraseBound(dimension);
    else
        writablePage(dimension / BoundsPerPage)
            .bounds[dimension % BoundsPerPage] = std::move(interval);
}

void BoxState::setBound(Dimension dimension, Interval interval)
{
    if (interval.isTop())
        eraseBound(dimension);
    else
        writablePage(dimension / BoundsPerPage)
            .bounds[dimension % BoundsPerPage] = std::move(interval);
    canonicalize(dimension);
}

const BoxState::BoundPageDirectory& BoxState::pageDirectory() const
{
    return config_.directoryCOW ? *cowBoundPages_ : boundPages_;
}

BoxState::BoundPageDirectory& BoxState::writablePageDirectory()
{
    if (!config_.directoryCOW)
        return boundPages_;
    if (!cowBoundPages_)
        cowBoundPages_ = std::make_shared<BoundPageDirectory>();
    else if (cowBoundPages_.use_count() != 1)
    {
        recordStorage(BoxStorageOperation::DirectoryDetach);
        cowBoundPages_ =
            std::make_shared<BoundPageDirectory>(*cowBoundPages_);
    }
    return *cowBoundPages_;
}

BoxState::BoundPageHashDirectory& BoxState::writableHashPageDirectory()
{
    if (!cowHashBoundPages_)
        cowHashBoundPages_ = std::make_shared<BoundPageHashDirectory>();
    else if (cowHashBoundPages_.use_count() != 1)
    {
        recordStorage(BoxStorageOperation::DirectoryDetach);
        cowHashBoundPages_ =
            std::make_shared<BoundPageHashDirectory>(*cowHashBoundPages_);
    }
    return *cowHashBoundPages_;
}

const Interval& BoxState::boundAt(Dimension dimension) const
{
    static const Interval top = Interval::top();
    const std::size_t pageIndex = dimension / BoundsPerPage;
    if (config_.hashDirectoryCOW)
    {
        const auto iterator = cowHashBoundPages_->find(pageIndex);
        if (iterator == cowHashBoundPages_->end())
        {
            recordStorage(BoxStorageOperation::PageLookupMiss);
            recordStorage(BoxStorageOperation::SlotLookupMiss);
            return top;
        }
        recordStorage(BoxStorageOperation::PageLookupHit);
        const auto& slot =
            iterator->second->bounds[dimension % BoundsPerPage];
        recordStorage(slot ? BoxStorageOperation::SlotLookupHit
                           : BoxStorageOperation::SlotLookupMiss);
        return slot ? *slot : top;
    }
    const BoundPageDirectory& pages = pageDirectory();
    const auto iterator =
        std::lower_bound(pages.begin(), pages.end(), pageIndex,
                         [](const BoundPageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (iterator == pages.end() || iterator->index != pageIndex)
    {
        recordStorage(BoxStorageOperation::PageLookupMiss);
        recordStorage(BoxStorageOperation::SlotLookupMiss);
        return top;
    }
    recordStorage(BoxStorageOperation::PageLookupHit);
    const auto& slot = iterator->page->bounds[dimension % BoundsPerPage];
    recordStorage(slot ? BoxStorageOperation::SlotLookupHit
                       : BoxStorageOperation::SlotLookupMiss);
    return slot ? *slot : top;
}

BoxState::BoundPage& BoxState::writablePage(std::size_t pageIndex)
{
    if (config_.hashDirectoryCOW)
    {
        BoundPageHashDirectory& pages = writableHashPageDirectory();
        auto [iterator, inserted] =
            pages.try_emplace(pageIndex, std::make_shared<BoundPage>());
        if (inserted)
            recordStorage(BoxStorageOperation::PageCreate);
        else if (iterator->second.use_count() != 1)
        {
            const std::size_t sharingFanout = iterator->second.use_count();
            recordStorage(BoxStorageOperation::PageDetach);
            if (config_.storageTelemetry)
                config_.storageTelemetry->recordPageDetach(
                    config_.storageRole, sharingFanout);
            iterator->second =
                std::make_shared<BoundPage>(*iterator->second);
        }
        return *iterator->second;
    }
    BoundPageDirectory& pages = writablePageDirectory();
    auto iterator =
        std::lower_bound(pages.begin(), pages.end(), pageIndex,
                         [](const BoundPageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (iterator == pages.end() || iterator->index != pageIndex)
    {
        recordStorage(BoxStorageOperation::PageCreate);
        if (config_.storageTelemetry)
            config_.storageTelemetry->recordDirectoryShift(
                config_.storageRole,
                static_cast<std::size_t>(pages.end() - iterator));
        iterator = pages.insert(
            iterator, {pageIndex, std::make_shared<BoundPage>()});
    }
    else if (iterator->page.use_count() != 1)
    {
        const std::size_t sharingFanout = iterator->page.use_count();
        recordStorage(BoxStorageOperation::PageDetach);
        if (config_.storageTelemetry)
            config_.storageTelemetry->recordPageDetach(config_.storageRole,
                                                       sharingFanout);
        iterator->page = std::make_shared<BoundPage>(*iterator->page);
    }
    return *iterator->page;
}

void BoxState::eraseBound(Dimension dimension)
{
    const std::size_t pageIndex = dimension / BoundsPerPage;
    if (config_.hashDirectoryCOW)
    {
        const auto existing = cowHashBoundPages_->find(pageIndex);
        if (existing == cowHashBoundPages_->end())
            return;
        const std::size_t offset = dimension % BoundsPerPage;
        if (!existing->second->bounds[offset])
            return;
        BoundPageHashDirectory& pages = writableHashPageDirectory();
        auto iterator = pages.find(pageIndex);
        if (iterator->second.use_count() != 1)
        {
            const std::size_t sharingFanout = iterator->second.use_count();
            recordStorage(BoxStorageOperation::PageDetach);
            if (config_.storageTelemetry)
                config_.storageTelemetry->recordPageDetach(
                    config_.storageRole, sharingFanout);
            iterator->second =
                std::make_shared<BoundPage>(*iterator->second);
        }
        iterator->second->bounds[offset].reset();
        if (pageIsEmpty(*iterator->second))
        {
            recordStorage(BoxStorageOperation::PageErase);
            pages.erase(iterator);
        }
        return;
    }
    const BoundPageDirectory& existingPages = pageDirectory();
    auto existing =
        std::lower_bound(existingPages.begin(), existingPages.end(), pageIndex,
                         [](const BoundPageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (existing == existingPages.end() || existing->index != pageIndex)
        return;
    const std::size_t offset = dimension % BoundsPerPage;
    if (!existing->page->bounds[offset])
        return;
    BoundPageDirectory& pages = writablePageDirectory();
    auto iterator =
        std::lower_bound(pages.begin(), pages.end(), pageIndex,
                         [](const BoundPageEntry& entry, std::size_t index) {
                             return entry.index < index;
                         });
    if (iterator->page.use_count() != 1)
    {
        const std::size_t sharingFanout = iterator->page.use_count();
        recordStorage(BoxStorageOperation::PageDetach);
        if (config_.storageTelemetry)
            config_.storageTelemetry->recordPageDetach(config_.storageRole,
                                                       sharingFanout);
        iterator->page = std::make_shared<BoundPage>(*iterator->page);
    }
    iterator->page->bounds[offset].reset();
    if (pageIsEmpty(*iterator->page))
    {
        recordStorage(BoxStorageOperation::PageErase);
        pages.erase(iterator);
    }
}

bool BoxState::pageIsEmpty(const BoundPage& page)
{
    return std::none_of(page.bounds.begin(), page.bounds.end(),
                        [](const auto& bound) { return bound.has_value(); });
}

std::vector<Dimension> BoxState::boundedDimensions() const
{
    std::vector<Dimension> dimensions;
    if (config_.hashDirectoryCOW)
    {
        for (const auto& [pageIndex, page] : *cowHashBoundPages_)
        {
            for (std::size_t offset = 0; offset < BoundsPerPage; ++offset)
            {
                const Dimension dimension =
                    pageIndex * BoundsPerPage + offset;
                if (dimension < environment_.size() && page->bounds[offset])
                    dimensions.push_back(dimension);
            }
        }
        std::sort(dimensions.begin(), dimensions.end());
        return dimensions;
    }
    for (const BoundPageEntry& entry : pageDirectory())
    {
        for (std::size_t offset = 0; offset < BoundsPerPage; ++offset)
        {
            const Dimension dimension = entry.index * BoundsPerPage + offset;
            if (dimension >= environment_.size())
                break;
            if (entry.page->bounds[offset])
                dimensions.push_back(dimension);
        }
    }
    return dimensions;
}

BoxStorageMetrics BoxState::storageMetrics() const
{
    BoxStorageMetrics metrics;
    metrics.environmentDimensions = environment_.size();
    if (config_.hashDirectoryCOW)
    {
        metrics.pageReferences = cowHashBoundPages_->size();
        metrics.allocatedSlots =
            cowHashBoundPages_->size() * BoundsPerPage;
        for (const auto& [pageIndex, page] : *cowHashBoundPages_)
        {
            (void)pageIndex;
            metrics.materializedBounds +=
                static_cast<std::size_t>(std::count_if(
                    page->bounds.begin(), page->bounds.end(),
                    [](const auto& bound) { return bound.has_value(); }));
        }
        return metrics;
    }
    const BoundPageDirectory& pages = pageDirectory();
    metrics.pageReferences = pages.size();
    metrics.allocatedSlots = pages.size() * BoundsPerPage;
    for (const BoundPageEntry& entry : pages)
    {
        metrics.materializedBounds += static_cast<std::size_t>(std::count_if(
            entry.page->bounds.begin(), entry.page->bounds.end(),
            [](const auto& bound) { return bound.has_value(); }));
    }
    return metrics;
}

void BoxState::sampleResidentStorage() const
{
    if (config_.storageTelemetry)
        config_.storageTelemetry->recordResidentShape(config_.storageRole,
                                                      storageMetrics());
}

void BoxState::makeBottom()
{
    bottom_ = true;
    if (config_.hashDirectoryCOW)
        writableHashPageDirectory().clear();
    else
        writablePageDirectory().clear();
}

void BoxState::report(OperationKind operation, ApproximationKind approximation,
                      std::string reason, bool best) const
{
    recordOperation(operation, approximation, best, reason);
    if (config_.diagnostics)
        config_.diagnostics->report(
            {operation, approximation, std::move(reason)});
}

void BoxState::profileStorage(BoxStorageOperation operation) const
{
    if (!config_.storageTelemetry)
        return;
    if (config_.storageTelemetry->record(config_.storageRole, operation))
        config_.storageTelemetry->recordShape(config_.storageRole,
                                              storageMetrics());
}

void BoxState::recordStorage(BoxStorageOperation operation) const
{
    if (config_.storageTelemetry)
        config_.storageTelemetry->record(config_.storageRole, operation,
                                         false);
}

} // namespace SVF::AbstractDomain
