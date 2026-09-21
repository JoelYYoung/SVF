//===- NumericalDomain.h -- Numerical properties and Box domain -*- C++ -*-===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//
// Contributors: Jiawei Wang, Xiao Cheng, Jiawei Yang
//
//===----------------------------------------------------------------------===//

#ifndef SVF_AE_NUMERICAL_DOMAIN_H
#define SVF_AE_NUMERICAL_DOMAIN_H

#include "AE/Core/AbstractDomain.h"
#include "AE/Core/Variable.h"

#include <gmpxx.h>
#include <mpfr.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace SVF::AbstractDomain
{

class Integer
{
public:
    Integer();
    explicit Integer(std::int64_t value);
    explicit Integer(const std::string& value);

    const mpz_class& value() const
    {
        return value_;
    }
    std::string toString() const;

    friend bool operator==(const Integer& lhs, const Integer& rhs)
    {
        return lhs.value_ == rhs.value_;
    }

private:
    mpz_class value_;
};

class Rational
{
public:
    Rational();
    explicit Rational(std::int64_t value);
    explicit Rational(const Integer& value);
    explicit Rational(const std::string& value);
    Rational(const Integer& numerator, const Integer& denominator);

    static Rational fromRaw(const mpq_class& value);
    static Rational fromDouble(double value);

    const mpq_class& value() const
    {
        return value_;
    }
    bool isZero() const
    {
        return value_ == 0;
    }
    bool isInteger() const;
    std::int64_t toInt64() const;
    double toDouble() const;
    int sign() const
    {
        return mpq_sgn(value_.get_mpq_t());
    }
    std::string toString() const;

    Rational floor() const;
    Rational ceil() const;
    Rational dividedByPowerOfTwo(unsigned exponent) const;
    Rational& assignSum(const Rational& lhs, const Rational& rhs);
    Rational& divideByPowerOfTwoInPlace(unsigned exponent);

    Rational& operator+=(const Rational& rhs);
    Rational& operator-=(const Rational& rhs);
    Rational& operator*=(const Rational& rhs);
    Rational& operator/=(const Rational& rhs);

    friend Rational operator+(Rational lhs, const Rational& rhs)
    {
        return lhs += rhs;
    }
    friend Rational operator-(Rational lhs, const Rational& rhs)
    {
        return lhs -= rhs;
    }
    friend Rational operator*(Rational lhs, const Rational& rhs)
    {
        return lhs *= rhs;
    }
    friend Rational operator/(Rational lhs, const Rational& rhs)
    {
        return lhs /= rhs;
    }
    friend Rational operator-(const Rational& value)
    {
        return Rational::fromRaw(-value.value_);
    }

    friend bool operator==(const Rational& lhs, const Rational& rhs)
    {
        return lhs.value_ == rhs.value_;
    }
    friend bool operator!=(const Rational& lhs, const Rational& rhs)
    {
        return !(lhs == rhs);
    }
    friend bool operator<(const Rational& lhs, const Rational& rhs)
    {
        return lhs.value_ < rhs.value_;
    }
    friend bool operator<=(const Rational& lhs, const Rational& rhs)
    {
        return lhs.value_ <= rhs.value_;
    }
    friend bool operator>(const Rational& lhs, const Rational& rhs)
    {
        return rhs < lhs;
    }
    friend bool operator>=(const Rational& lhs, const Rational& rhs)
    {
        return rhs <= lhs;
    }

private:
    explicit Rational(mpq_class value, int);
    mpq_class value_;
};

/// An ordered extended-rational endpoint.  For a finite upper bound, strict
/// means "< value" and non-strict means "<= value".  At an equal numeric
/// value a strict bound is tighter than a non-strict one.
class Bound
{
public:
    enum class Kind
    {
        MinusInfinity,
        Finite,
        PlusInfinity
    };

    Bound();
    static Bound minusInfinity();
    static Bound finite(Rational value, bool strict = false);
    static Bound plusInfinity();

    Kind kind() const
    {
        return kind_;
    }
    bool isFinite() const
    {
        return kind_ == Kind::Finite;
    }
    bool isMinusInfinity() const
    {
        return kind_ == Kind::MinusInfinity;
    }
    bool isPlusInfinity() const
    {
        return kind_ == Kind::PlusInfinity;
    }
    const Rational& value() const;
    bool isStrict() const
    {
        return strict_;
    }

    /// Ordering used by upper bounds: tighter/smaller first.
    static int compare(const Bound& lhs, const Bound& rhs);
    static Bound min(const Bound& lhs, const Bound& rhs);
    static Bound max(const Bound& lhs, const Bound& rhs);
    static Bound add(const Bound& lhs, const Bound& rhs);
    Bound& assignSum(const Bound& lhs, const Bound& rhs);
    Bound& divideByTwoInPlace();
    static Bound divideByTwo(const Bound& bound);
    static Bound divideByPositive(const Bound& bound, const Rational& divisor);

    std::string toString() const;

    friend bool operator==(const Bound& lhs, const Bound& rhs)
    {
        return compare(lhs, rhs) == 0;
    }
    friend bool operator!=(const Bound& lhs, const Bound& rhs)
    {
        return !(lhs == rhs);
    }
    friend bool operator<(const Bound& lhs, const Bound& rhs)
    {
        return compare(lhs, rhs) < 0;
    }
    friend bool operator<=(const Bound& lhs, const Bound& rhs)
    {
        return compare(lhs, rhs) <= 0;
    }

private:
    Bound(Kind kind, Rational value, bool strict);

    Kind kind_ = Kind::PlusInfinity;
    Rational value_;
    bool strict_ = false;
};

class Interval
{
public:
    Interval();
    Interval(Bound lower, Bound upper);

    static Interval top();
    static Interval bottom();
    static Interval singleton(const Rational& value);
    static Interval closed(const Rational& lower, const Rational& upper);

    const Bound& lower() const
    {
        return lower_;
    }
    const Bound& upper() const
    {
        return upper_;
    }
    bool isTop() const;
    bool isBottom() const;
    bool isSingleton() const;
    bool isZero() const;
    bool contains(const Rational& value) const;
    bool isSubsetOf(const Interval& other) const;
    const Rational& singletonValue() const;
    void joinWith(const Interval& other);
    void meetWith(const Interval& other);
    void widenWith(const Interval& next);
    void narrowWith(const Interval& next);
    std::string toString() const;

    friend bool operator==(const Interval& lhs, const Interval& rhs)
    {
        return lhs.lower_ == rhs.lower_ && lhs.upper_ == rhs.upper_;
    }
    friend bool operator!=(const Interval& lhs, const Interval& rhs)
    {
        return !(lhs == rhs);
    }

private:
    Bound lower_;
    Bound upper_;
};

Interval add(const Interval& lhs, const Interval& rhs);
Interval subtract(const Interval& lhs, const Interval& rhs);
Interval multiply(const Interval& lhs, const Interval& rhs);
Interval divide(const Interval& lhs, const Interval& rhs,
                bool integerDivision = true);
Interval remainder(const Interval& lhs, const Interval& rhs);
Interval bitwiseAnd(const Interval& lhs, const Interval& rhs);
Interval bitwiseOr(const Interval& lhs, const Interval& rhs);
Interval bitwiseXor(const Interval& lhs, const Interval& rhs);
Interval shiftLeft(const Interval& lhs, const Interval& rhs);
Interval shiftRight(const Interval& lhs, const Interval& rhs);
Interval equalTo(const Interval& lhs, const Interval& rhs);
Interval notEqualTo(const Interval& lhs, const Interval& rhs);
Interval lessThan(const Interval& lhs, const Interval& rhs);
Interval lessEqual(const Interval& lhs, const Interval& rhs);
Interval greaterThan(const Interval& lhs, const Interval& rhs);
Interval greaterEqual(const Interval& lhs, const Interval& rhs);
/// Complete mathematical range of a fixed-width signed or unsigned integer.
Interval integerRange(unsigned bitWidth, bool isSigned);
/// Convert a floating interval to a fixed-width integer using LLVM's
/// round-toward-zero semantics. If the operand may produce poison because it
/// is non-finite or outside the destination range, return that complete range.
Interval floatToInteger(const Interval& operand, unsigned bitWidth,
                        bool isSigned);

enum class RoundingMode
{
    NearestTiesToEven,
    TowardZero,
    TowardPositive,
    TowardNegative
};

class MpfrValue
{
public:
    explicit MpfrValue(mpfr_prec_t precision);
    MpfrValue(const MpfrValue& rhs);
    MpfrValue(MpfrValue&& rhs) noexcept;
    MpfrValue& operator=(const MpfrValue& rhs);
    MpfrValue& operator=(MpfrValue&& rhs) noexcept;
    ~MpfrValue();

    mpfr_ptr raw()
    {
        return value_;
    }
    mpfr_srcptr raw() const
    {
        return value_;
    }
    mpfr_prec_t precision() const
    {
        return mpfr_get_prec(value_);
    }

    void set(const Rational& value, mpfr_rnd_t rounding);
    Rational toRational() const;

private:
    mpfr_t value_;
};

/// Ground MPFR operations used at the floating-semantics boundary.  The
/// returned rational is the exact dyadic value of the rounded MPFR result.
class FloatSemantics
{
public:
    static Rational add(const Rational& lhs, const Rational& rhs,
                        unsigned significandBits, RoundingMode rounding);
    static Rational subtract(const Rational& lhs, const Rational& rhs,
                             unsigned significandBits, RoundingMode rounding);
    static Rational multiply(const Rational& lhs, const Rational& rhs,
                             unsigned significandBits, RoundingMode rounding);
    static Rational divide(const Rational& lhs, const Rational& rhs,
                           unsigned significandBits, RoundingMode rounding);

private:
    enum class BinaryOperation
    {
        Add,
        Subtract,
        Multiply,
        Divide
    };

    static Rational evaluate(BinaryOperation operation, const Rational& lhs,
                             const Rational& rhs, unsigned significandBits,
                             RoundingMode rounding);
};

class LinearExpression;
class TreeExpression;
class LinearConstraint;
class TreeConstraint;
struct LinearAssignment;
struct TreeAssignment;
struct WideningPolicy;
using LinearAssignmentList = std::vector<LinearAssignment>;
using TreeAssignmentList = std::vector<TreeAssignment>;
using LinearConstraintSet = std::vector<LinearConstraint>;

enum class ApproximationKind
{
    Exact,
    SoundOverApproximation,
    UnsupportedFallback
};

enum class OperationKind
{
    Assignment,
    Assumption,
    Substitution,
    Forget,
    Join,
    Meet,
    Widening,
    Narrowing,
    TopologicalClosure,
    Canonicalization,
    Expand,
    Fold,
    GeneratorImport,
    GeneratorExport
};

/// APRON-style information about the most recently completed mutating
/// operation. `exact` means that no semantic approximation beyond the
/// selected abstract domain was introduced. `best` means that the operation
/// used the strongest implemented transformer for that domain and syntax.
struct OperationMetadata
{
    OperationKind operation = OperationKind::Assignment;
    ApproximationKind approximation = ApproximationKind::Exact;
    bool exact = true;
    bool best = true;
    std::string reason;
};

struct Diagnostic
{
    OperationKind operation;
    ApproximationKind approximation;
    std::string reason;
};

class DiagnosticSink
{
public:
    virtual ~DiagnosticSink() = default;
    virtual void report(const Diagnostic& diagnostic) = 0;
};

/// Common interface for numerical abstract properties. The representation and
/// lattice algorithms remain domain-specific; clients such as the SVF adapter
/// and test oracles only need this transfer/query surface.
class NumericalDomain : public AbstractDomain
{
public:
    using RawBuffer = std::vector<std::uint8_t>;

    ~NumericalDomain() override = default;

    /// Return a deterministic semantic hash. Compatible properties that are
    /// equivalent according to isEquivalentTo() have the same hash. Hash
    /// equality is not a substitute for an exact equivalence check.
    std::uint64_t hash() const;

    /// Serialize the domain kind, operation-relevant configuration, and
    /// canonical mathematical state into a versioned binary buffer.
    /// Diagnostic sinks are observational and are not serialized.
    RawBuffer serializeRaw() const;

    /// Restore a Box, Octagon, or Convex Polyhedra property from serializeRaw().
    /// Malformed, truncated, corrupt, or unsupported data is rejected.
    static std::unique_ptr<NumericalDomain> deserializeRaw(
        const RawBuffer& buffer);

    const OperationMetadata& lastOperation() const
    {
        return lastOperation_;
    }
    virtual void assign(Variable target,
                        const LinearExpression& expression) = 0;
    virtual void assign(Variable target, const TreeExpression& expression) = 0;
    /// Assign every target simultaneously. Every right-hand side reads the
    /// same incoming state, including old values of all assigned targets.
    virtual void assignParallel(const LinearAssignmentList& assignments) = 0;
    virtual void assignParallel(const TreeAssignmentList& assignments);
    /// Compute the preimage of this post-state under target := expression.
    /// This is APRON's substitute operation, not a forward strong update.
    virtual void substitute(Variable target,
                            const LinearExpression& expression) = 0;
    void substitute(Variable target, const TreeExpression& expression);
    /// Simultaneous backward substitution. Every replacement is interpreted
    /// over the same pre-state, including cyclic replacements.
    virtual void substituteParallel(
        const LinearAssignmentList& assignments) = 0;
    void substituteParallel(const TreeAssignmentList& assignments);
    virtual void assume(const LinearConstraint& constraint) = 0;
    virtual void assume(const TreeConstraint& constraint) = 0;
    virtual void forget(Variable variable) = 0;
    /// Duplicate a summary variable into fresh variables. Every copy has the
    /// source variable's relations with all other variables, while the
    /// expanded variables remain mutually unrelated except where those
    /// duplicated relations logically imply otherwise. This is APRON's
    /// expand operation.
    virtual void expand(Variable source,
                        const std::vector<Variable>& copies) = 0;
    /// Merge several materialized variables into `target` by taking the
    /// abstract hull of every possible representative, then remove the other
    /// variables. This is APRON's fold operation.
    virtual void fold(Variable target, const std::vector<Variable>& folded) = 0;

    /// Assume every constraint, letting them propagate into each other until
    /// the state stops moving.
    ///
    /// Assuming them one at a time is weaker than a client of a guard such as
    /// `a && b && c` expects: a bound learned from the last constraint cannot
    /// flow back into the first. A domain that is exact on linear constraints
    /// settles in one pass and pays only the comparison; a non-relational or
    /// octagonal domain is the reason this exists.
    virtual void assumeAll(const LinearConstraintSet& constraints);

    virtual CheckResult entails(const LinearConstraint& constraint) const = 0;
    virtual Interval bound(Variable variable) const = 0;
    /// Bound a complete affine expression using the relational backend, not
    /// merely interval arithmetic over its individual variables.
    virtual Interval bound(const LinearExpression& expression) const = 0;
    /// Affine integer/real trees use bound(LinearExpression). Nonlinear and
    /// finite IEEE trees use sound interval evaluation with outward rounding;
    /// exceptional IEEE outcomes that cannot be represented numerically lose
    /// the affected bound to top.
    Interval bound(const TreeExpression& expression) const;
    virtual LinearConstraintSet toConstraints() const = 0;

    /// Replace strict boundaries by non-strict boundaries. This is the
    /// topological closure operation, not DBM/polyhedral normalization.
    virtual void close() = 0;
    /// Materialize the backend's canonical representation and remove semantic
    /// redundancy where the representation supports it.
    virtual void canonicalize() = 0;
    /// Dense native representations use canonicalization as their minimize
    /// operation.
    void minimize()
    {
        canonicalize();
    }

protected:
    /// Evaluate nonlinear and finite IEEE trees by sound interval semantics,
    /// applying each IEEE node's requested rounding mode at its endpoints.
    /// Exceptional IEEE outcomes conservatively produce top.
    Interval evaluateTreeExpression(const TreeExpression& expression) const;
    /// Necessary affine consequences of a nonlinear tree guard. The result
    /// may be empty when the guard cannot safely refine the selected domain.
    LinearConstraintSet treeConstraintConsequences(
        const TreeConstraint& constraint) const;
    /// Strongly update a target from an already-computed interval. This is
    /// used to preserve simultaneous semantics for nonlinear tree batches.
    virtual void assignInterval(Variable target, const Interval& value);
    void recordOperation(OperationKind operation,
                         ApproximationKind approximation, bool best,
                         std::string reason = {}) const;

private:
    mutable OperationMetadata lastOperation_;
};

struct BoxSemanticConfig
{
    bool integerTightening = true;
    std::shared_ptr<DiagnosticSink> diagnostics;

    bool operationCompatible(const BoxSemanticConfig& other) const
    {
        return integerTightening == other.integerTightening;
    }
};

#ifdef SVF_BOX_STORAGE_TELEMETRY
enum class BoxStorageEventKind
{
    PageAllocate,
    PageDetach,
    PageRelease,
    PageWriteUnique,
    PageEraseUnique,
    JoinSharedPage,
    JoinMaterializedPage,
    DirectoryDetach,
    DirectoryChunkDetach,
    Count
};

struct BoxStorageEvent
{
    BoxStorageEventKind kind = BoxStorageEventKind::PageAllocate;
    std::uint64_t sequence = 0;
    std::uint64_t pageId = 0;
    std::uint64_t parentPageId = 0;
    std::size_t pageIndex = 0;
    std::size_t occupiedSlots = 0;
    std::size_t directoryEntries = 0;
    /// Zero outside a logical Box mutation observed by BoxMutationEventSink.
    std::uint64_t mutationEpoch = 0;
    std::uint64_t stateId = 0;
};

struct BoxStoragePageSnapshot
{
    std::uint64_t pageId = 0;
    std::uint64_t parentPageId = 0;
    std::size_t pageIndex = 0;
    std::size_t referenceCount = 0;
    std::size_t occupiedSlots = 0;
    std::size_t rationalUsedLimbBytes = 0;
    std::string canonicalContent;
    std::size_t shallowBytes = 0;
    bool directIndexedSlots = false;
};

struct BoxStorageDirectoryChunkSnapshot
{
    std::uintptr_t chunkId = 0;
    std::size_t chunkIndex = 0;
    std::size_t referenceCount = 0;
    std::size_t shallowBytes = 0;
    std::size_t pageEntries = 0;
};

struct BoxStorageSnapshot
{
    bool bottom = false;
    std::uintptr_t directoryId = 0;
    std::size_t directoryReferenceCount = 0;
    std::size_t directoryEntries = 0;
    std::size_t directoryCapacity = 0;
    std::size_t slotsPerPage = 0;
    std::size_t directoryAllocatedBytes = 0;
    std::size_t directoryRootAllocatedBytes = 0;
    std::size_t pageShallowBytes = 0;
    std::size_t occupiedIndexShallowBytes = 0;
    std::size_t occupiedIntervalShallowBytes = 0;
    std::size_t rationalUsedLimbBytes = 0;
    std::size_t canonicalContentBytes = 0;
    std::vector<BoxStorageDirectoryChunkSnapshot> directoryChunks;
    std::vector<BoxStoragePageSnapshot> pages;
};

using BoxStorageEventSink = void (*)(const BoxStorageEvent&);

/// Successful physical slot operations, not logical AE statements. Counts
/// include temporary carriers. Byte counts exclude GMP limbs and allocators.
enum class BoxStorageWorkKind
{
    Clone, Grow, Shrink, Promote, Demote, Insert, Update, Erase, Count
};

struct BoxStorageWorkEvent
{
    BoxStorageWorkKind kind;
    std::size_t occupiedSlots;
    std::size_t copiedSlots;
    /// Slots relocated by reserve/insert/erase; not a promise of C++ move
    /// rather than copy construction by the standard library.
    std::size_t relocatedSlots;
    std::size_t allocatedSlotBytes;
    std::size_t overlappingSlotBytes;
    bool directIndexed;
    /// Filled by emitStorageWork when slot work occurs inside a mutation.
    std::uint64_t mutationEpoch = 0;
    std::uint64_t stateId = 0;
};

using BoxStorageWorkSink = void (*)(const BoxStorageWorkEvent&);

enum class BoxMutationKind
{
    Assignment,
    ParallelAssignment,
    Substitution,
    Assumption,
    Forget,
    Expand,
    Fold,
    Close,
    Canonicalize,
    Join,
    Meet,
    Widen,
    Narrow,
    DirectSet,
    DirectErase,
    Count
};

enum class BoxMutationPhase
{
    Begin,
    End
};

/// One diagnostic logical-mutation epoch. Nested operations on the same Box
/// are folded into their outer epoch. On End, changedVariables points to
/// callback-lifetime storage and lists exactly the typed variables whose
/// interval changed; it is never a physical-page approximation.
struct BoxMutationEvent
{
    BoxMutationKind kind;
    BoxMutationPhase phase;
    std::uint64_t sequence;
    std::uint64_t epoch;
    std::uint64_t stateId;
    std::uint64_t relatedStateId;
    bool beforeBottom;
    bool afterBottom;
    const Variable* changedVariables;
    /// Parallel callback-lifetime array: nonzero means the changed variable
    /// remains explicitly constrained after the epoch.
    const std::uint8_t* changedVariablesConstrainedAfter;
    std::size_t changedVariableCount;
    /// Variables on which the implementation attempted a physical update,
    /// including semantic no-ops. This is a superset of changedVariables.
    const Variable* touchedVariables;
    std::size_t touchedVariableCount;
};

using BoxMutationEventSink = void (*)(const BoxMutationEvent&);

enum class BoxStateEventKind
{
    Create,
    CopyConstruct,
    MoveConstruct,
    CopyAssign,
    MoveAssign,
    Destroy
};

/// Lifecycle and COW-sharing event. IDs are monotone within one process run;
/// sourceStateId is nonzero for copy/move construction and assignment.
struct BoxStateEvent
{
    BoxStateEventKind kind;
    std::uint64_t sequence;
    std::uint64_t stateId;
    std::uint64_t sourceStateId;
    /// Nonzero only when an assignment replaces the active mutation's state.
    std::uint64_t mutationEpoch;
    bool bottom;
};

using BoxStateEventSink = void (*)(const BoxStateEvent&);
#endif

/// Non-relational numerical property with finite non-Top support over stable
/// typed Variables. Variable IDs are the global sparse page coordinates.
class BoxDomain final : public NumericalDomain
{
    friend class BoxAddressDomain;

public:
    using NumericalDomain::assignParallel;
    using NumericalDomain::bound;
    using NumericalDomain::substitute;
    using NumericalDomain::substituteParallel;

    static BoxDomain top(const BoxSemanticConfig& config = {});
    static BoxDomain bottom(const BoxSemanticConfig& config = {});
    /// Runtime library identity for representation experiments, not semantics.
    static const char* storageRepresentation() noexcept;
    static const char* pageInterningPolicy() noexcept;
#ifdef SVF_BOX_PAGE_INTERNING
    /// Experimental, thread-confined, analysis-scoped weak content pool.
    /// The budget counts index entries, not bytes or retained strong pages.
    class PagePool
    {
    public:
        struct Statistics
        {
            std::uint64_t publications = 0, candidates = 0, probes = 0;
            std::uint64_t hits = 0, comparisons = 0, expired = 0;
            std::uint64_t evictions = 0, frozenDetaches = 0;
            std::size_t entries = 0, peakEntries = 0;
        };
        explicit PagePool(bool afterWrite, std::size_t capacity = 32768,
                          bool forceHashCollision = false);
        ~PagePool();
        PagePool(const PagePool&) = delete;
        PagePool& operator=(const PagePool&) = delete;
        Statistics statistics() const;
        void sweepExpired();

    private:
        friend class BoxDomain;
        struct Impl;
        std::unique_ptr<Impl> impl_;
        PagePool* previous_;
        static thread_local PagePool* active_;
    };
    /// Physical normalization only; does not change logical bounds or metadata.
    void internPendingPages();
#endif
    static BoxDomain fromConstraints(const LinearConstraintSet& constraints,
                                     const BoxSemanticConfig& config = {});

    BoxDomain(const BoxDomain& other);
#ifdef SVF_BOX_STORAGE_TELEMETRY
    BoxDomain(BoxDomain&& other) noexcept;
    ~BoxDomain() override;
#else
    BoxDomain(BoxDomain&& other) noexcept = default;
#endif
#ifdef SVF_BOX_STORAGE_TELEMETRY
    BoxDomain& operator=(const BoxDomain& other);
    BoxDomain& operator=(BoxDomain&& other) noexcept;
#else
    BoxDomain& operator=(const BoxDomain& other) = default;
    BoxDomain& operator=(BoxDomain&& other) noexcept = default;
#endif

    DomainKind kind() const noexcept override
    {
        return DomainKind::Box;
    }
    std::unique_ptr<AbstractDomain> clone() const override;
    const BoxSemanticConfig& config() const
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
    void assumeAll(const LinearConstraintSet& constraints) override;
    void forget(Variable variable) override;
    void expand(Variable source, const std::vector<Variable>& copies) override;
    void fold(Variable target, const std::vector<Variable>& folded) override;

    CheckResult entails(const LinearConstraint& constraint) const override;
    Interval bound(Variable variable) const override;
    Interval bound(const LinearExpression& expression) const override;
    /// Variables whose bounds are represented explicitly because they are
    /// stricter than the analysis-wide Top default. This is a storage
    /// observation for sparse scheduling; absence never means undefined.
    std::vector<Variable> constrainedVariables() const;
    std::vector<Variable> constrainedVariablesBefore(
        Variable upperBound) const;
    LinearConstraintSet toConstraints() const override;
    void close() override;
    void canonicalize() override;

#ifdef SVF_BOX_STORAGE_TELEMETRY
    /// Installs a process-wide diagnostic sink. The caller owns the sink and
    /// must keep it valid until replacing it with nullptr.
    static void setStorageEventSink(BoxStorageEventSink sink) noexcept;
    static void setStorageWorkSink(BoxStorageWorkSink sink) noexcept;
    static void setMutationEventSink(BoxMutationEventSink sink) noexcept;
    static void setStateEventSink(BoxStateEventSink sink) noexcept;
    BoxStorageSnapshot storageSnapshot() const;
#endif

    BoxDomain join(const BoxDomain& other) const;
    BoxDomain meet(const BoxDomain& other) const;
    BoxDomain widen(const BoxDomain& next) const;
    BoxDomain widen(const BoxDomain& next, const WideningPolicy& policy) const;
    BoxDomain narrow(const BoxDomain& next) const;

private:
    static constexpr std::size_t BoundsPerPage = 8;
    static constexpr std::size_t DirectoryPagesPerChunk = 8;

    struct BoundSlot
    {
        Variable variable;
        Interval interval;

        friend bool operator==(const BoundSlot& lhs, const BoundSlot& rhs)
        {
            return lhs.variable == rhs.variable && lhs.interval == rhs.interval;
        }
    };

    struct BoundPage
    {
        /// Stable offsets are independent of physical placement. Packed pages
        /// construct only present slots; vector capacity is included in census.
        struct Slots
        {
#if defined(SVF_BOX_PACKED_PAGES) || defined(SVF_BOX_ADAPTIVE_PAGES)
            std::vector<BoundSlot> values;
            unsigned mask = 0;
#ifdef SVF_BOX_ADAPTIVE_PAGES
            // Density-only control: promote at 6/8, demote at 2/8. Fixed
            // thresholds deliberately leave a wide hysteresis band. No program
            // identity or future trace informs this experimental policy.
            bool direct = false;

            void repack(bool nextDirect)
            {
#ifdef SVF_BOX_STORAGE_TELEMETRY
                const auto beforeBytes = allocatedBytes();
                const auto copied = size();
#endif
                std::vector<BoundSlot> next;
                next.reserve(nextDirect ? BoundsPerPage : size());
                for (std::size_t offset = 0; offset < BoundsPerPage; ++offset)
                {
                    if (const BoundSlot* slot = find(offset))
                        next.push_back(*slot);
                    else if (nextDirect)
                        next.push_back({Variable(0), Interval::top()});
                }
                values.swap(next);
                direct = nextDirect;
#ifdef SVF_BOX_STORAGE_TELEMETRY
                BoxDomain::emitStorageWork({nextDirect ? BoxStorageWorkKind::Promote :
                                            BoxStorageWorkKind::Demote,
                                            size(), copied, 0, allocatedBytes(),
                                            beforeBytes + allocatedBytes(), direct
                                           });
#endif
            }
#endif

            bool directIndexed() const
            {
#ifdef SVF_BOX_ADAPTIVE_PAGES
                return direct;
#else
                return false;
#endif
            }

            std::size_t rank(std::size_t offset) const
            {
                unsigned bits = mask & ((1u << offset) - 1);
                bits -= (bits >> 1) & 0x55u;
                bits = (bits & 0x33u) + ((bits >> 2) & 0x33u);
                return (bits + (bits >> 4)) & 0x0fu;
            }
            const BoundSlot* find(std::size_t offset) const
            {
                return mask & (1u << offset) ?
                       &values[directIndexed() ? offset : rank(offset)] : nullptr;
            }
            void set(std::size_t offset, BoundSlot value)
            {
#ifdef SVF_BOX_ADAPTIVE_PAGES
                if (direct)
                {
#ifdef SVF_BOX_STORAGE_TELEMETRY
                    const bool existed = mask & (1u << offset);
#endif
                    values[offset] = std::move(value);
                    mask |= 1u << offset;
#ifdef SVF_BOX_STORAGE_TELEMETRY
                    BoxDomain::emitStorageWork({existed ? BoxStorageWorkKind::Update :
                                                BoxStorageWorkKind::Insert, size(), 0, 0, 0, 0, true
                                               });
#endif
                    return;
                }
#endif
                const auto index = rank(offset);
                if (mask & (1u << offset))
                {
                    values[index] = std::move(value);
#ifdef SVF_BOX_STORAGE_TELEMETRY
                    BoxDomain::emitStorageWork({BoxStorageWorkKind::Update, size(), 0, 0, 0, 0, false});
#endif
                }
                else
                {
                    if (values.size() == values.capacity())
                    {
                        const auto capacity = values.capacity();
                        values.reserve(capacity == 0 ? 1 :
                                       capacity * 2 < BoundsPerPage ? capacity * 2 : BoundsPerPage);
#ifdef SVF_BOX_STORAGE_TELEMETRY
                        BoxDomain::emitStorageWork({BoxStorageWorkKind::Grow, size(), 0,
                                                    size(), allocatedBytes(),
                                                    capacity * sizeof(BoundSlot) + allocatedBytes(), false
                                                   });
#endif
                    }
                    values.insert(values.begin() + index, std::move(value));
                    mask |= 1u << offset;
#ifdef SVF_BOX_STORAGE_TELEMETRY
                    BoxDomain::emitStorageWork({BoxStorageWorkKind::Insert, size(), 0,
                                                size() - index - 1, 0, 0, false
                                               });
#endif
                }
#ifdef SVF_BOX_ADAPTIVE_PAGES
                if (values.size() >= 6)
                    repack(true);
#endif
            }
            void erase(std::size_t offset)
            {
                if (!(mask & (1u << offset)))
                    return;
#ifdef SVF_BOX_ADAPTIVE_PAGES
                if (direct)
                {
                    values[offset] = {Variable(0), Interval::top()};
                    mask &= ~(1u << offset);
#ifdef SVF_BOX_STORAGE_TELEMETRY
                    BoxDomain::emitStorageWork({BoxStorageWorkKind::Erase, size(), 0, 0, 0, 0, true});
#endif
                    if (size() <= 2)
                        repack(false);
                    return;
                }
#endif
                const auto index = rank(offset);
                values.erase(values.begin() + index);
                mask &= ~(1u << offset);
#ifdef SVF_BOX_STORAGE_TELEMETRY
                BoxDomain::emitStorageWork({BoxStorageWorkKind::Erase, size(), 0,
                                            size() - index, 0, 0, false
                                           });
#endif
                // Hysteresis avoids allocating on every erase/reinsert pair.
                // A COW clone also copies only live slots, not spare capacity.
                if (values.size() * 4 <= values.capacity())
                {
#ifdef SVF_BOX_STORAGE_TELEMETRY
                    const auto beforeBytes = allocatedBytes();
#endif
                    std::vector<BoundSlot>(values).swap(values);
#ifdef SVF_BOX_STORAGE_TELEMETRY
                    BoxDomain::emitStorageWork({BoxStorageWorkKind::Shrink, size(), size(),
                                                0, allocatedBytes(), beforeBytes + allocatedBytes(), false
                                               });
#endif
                }
            }
            std::size_t size() const
            {
                return directIndexed() ? rank(BoundsPerPage) : values.size();
            }
            std::size_t allocatedBytes() const
            {
                return values.capacity() * sizeof(BoundSlot);
            }
            bool operator!=(const Slots& other) const
            {
#ifdef SVF_BOX_ADAPTIVE_PAGES
                if (mask != other.mask)
                    return true;
                // Hysteresis permits equal contents in different layouts.
                // Empty direct slots contain placeholders, never constraints.
                for (std::size_t offset = 0; offset < BoundsPerPage; ++offset)
                    if ((mask & (1u << offset)) &&
                            !(*find(offset) == *other.find(offset)))
                        return true;
                return false;
#else
                return mask != other.mask || values != other.values;
#endif
            }
#else
            std::array<std::optional<BoundSlot>, BoundsPerPage> values;

            bool directIndexed() const
            {
                return true;
            }

            const BoundSlot* find(std::size_t offset) const
            {
                return values[offset] ? &*values[offset] : nullptr;
            }
            void set(std::size_t offset, BoundSlot value)
            {
#ifdef SVF_BOX_STORAGE_TELEMETRY
                const bool existed = values[offset].has_value();
#endif
                values[offset] = std::move(value);
#ifdef SVF_BOX_STORAGE_TELEMETRY
                BoxDomain::emitStorageWork({existed ? BoxStorageWorkKind::Update :
                                            BoxStorageWorkKind::Insert, size(), 0, 0, 0, 0, true
                                           });
#endif
            }
            void erase(std::size_t offset)
            {
#ifdef SVF_BOX_STORAGE_TELEMETRY
                const bool existed = values[offset].has_value();
#endif
                values[offset].reset();
#ifdef SVF_BOX_STORAGE_TELEMETRY
                if (existed)
                    BoxDomain::emitStorageWork({BoxStorageWorkKind::Erase, size(), 0, 0, 0, 0, true});
#endif
            }
            std::size_t size() const
            {
                std::size_t count = 0;
                for (const auto& value : values)
                    count += value.has_value();
                return count;
            }
            std::size_t allocatedBytes() const
            {
                return 0;
            }
            bool operator!=(const Slots& other) const
            {
                return values != other.values;
            }
#endif
            BoundSlot* find(std::size_t offset)
            {
                return const_cast<BoundSlot*>(
                           static_cast<const Slots&>(*this).find(offset));
            }
        } bounds;
#ifdef SVF_BOX_PAGE_INTERNING
        // Nonzero pages are immutable even if only one strong owner remains.
        std::uint64_t internedScope = 0;
#endif
#ifdef SVF_BOX_STORAGE_TELEMETRY
        BoundPage() = default;
        BoundPage(const BoundPage&) = delete;
        BoundPage& operator=(const BoundPage&) = delete;
        std::uint64_t storageId = 0;
        std::uint64_t parentStorageId = 0;
        std::size_t storageIndex = 0;
        ~BoundPage();
#endif
    };

    struct BoundPageEntry
    {
        std::size_t index;
        std::shared_ptr<BoundPage> page;
    };

#ifdef SVF_BOX_WHOLE_DIRECTORY
    using BoundPageDirectory = std::vector<BoundPageEntry>;
#else
    struct BoundPageDirectoryChunk
    {
        std::array<std::shared_ptr<BoundPage>, DirectoryPagesPerChunk> pages;
    };

    struct BoundPageDirectoryEntry
    {
        std::size_t index;
        std::shared_ptr<BoundPageDirectoryChunk> chunk;
    };

    using BoundPageDirectory = std::vector<BoundPageDirectoryEntry>;
#endif
    static std::shared_ptr<BoundPageDirectory> emptyPageDirectory();
    static std::shared_ptr<BoundPageDirectory> makePageDirectory(
        const std::vector<BoundPageEntry>& pages);
    BoxDomain(BoxSemanticConfig config, bool bottom);

    const void* dynamicTypeToken() const noexcept override
    {
        return staticTypeToken<BoxDomain>();
    }
    bool hasCompatibleDomain(const AbstractDomain& other) const override;
    void joinDomain(const AbstractDomain& other) override;
    void meetDomain(const AbstractDomain& other) override;
    void widenDomain(const AbstractDomain& next) override;
    void narrowDomain(const AbstractDomain& next) override;
    bool isBottomDomain() const override;
    bool isTopDomain() const override;
    bool leqDomain(const AbstractDomain& other) const override;
    std::string domainToString() const override;

    const BoxDomain& requireBox(const AbstractDomain& other) const;
    const BoundPageDirectory& pageDirectory() const noexcept;
    BoundPageDirectory& writablePageDirectory();
    std::vector<BoundPageEntry> pageEntries() const;
    const Interval& boundAt(Variable variable) const;
    BoundPage& writablePage(std::size_t pageIndex);
#ifdef SVF_BOX_PAGE_INTERNING
    void markPageDirty(std::size_t pageIndex);
    void internAfterWrite();
#endif
#ifdef SVF_BOX_STORAGE_TELEMETRY
    class MutationScope
    {
    public:
        MutationScope(BoxDomain& state, BoxMutationKind kind,
                      const BoxDomain* related = nullptr);
        ~MutationScope();

        MutationScope(const MutationScope&) = delete;
        MutationScope& operator=(const MutationScope&) = delete;

        static void recordBefore(BoxDomain& state, Variable variable);
        static void recordBeforeClear(BoxDomain& state, Variable variable);
        static void recordReplacement(BoxDomain& state,
                                      const BoxDomain& replacement);
        static std::uint64_t activeEpoch() noexcept;
        static const BoxDomain* activeState() noexcept;

    private:
        BoxDomain* state_ = nullptr;
        BoxMutationEventSink sink_ = nullptr;
        BoxMutationKind kind_ = BoxMutationKind::DirectSet;
        std::uint64_t epoch_ = 0;
        std::uint64_t relatedStateId_ = 0;
        bool beforeBottom_ = false;
        std::map<Variable, Interval> initialValues_;
        std::vector<Variable> touchedVariables_;

        static thread_local std::vector<MutationScope*> active_;
    };

    static std::shared_ptr<BoundPage> allocatePage(std::size_t pageIndex);
    static std::shared_ptr<BoundPage> clonePage(const BoundPage& source,
            BoxStorageEventKind reason);
    static void emitStorageEvent(BoxStorageEventKind kind,
                                 const BoundPage& page,
                                 std::uint64_t parentPageId = 0) noexcept;
    static void emitDirectoryDetach(std::size_t directoryEntries) noexcept;
    static void emitDirectoryChunkDetach(std::size_t pageEntries) noexcept;
    static void emitStorageWork(const BoxStorageWorkEvent& event) noexcept;
    static void emitStateEvent(BoxStateEventKind kind, const BoxDomain& state,
                               const BoxDomain* source = nullptr) noexcept;
    static std::size_t occupiedSlots(const BoundPage& page) noexcept;
#endif
    void eraseBound(Variable variable);
    static bool pageIsEmpty(const BoundPage& page);
    std::vector<Variable> boundedVariables() const;
    std::vector<Variable> boundedVariablesBefore(
        std::uint32_t upperBound) const;
    void makeBottom();
    void canonicalize(Variable variable);
    void setBound(Variable variable, Interval interval);
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason, bool best = true) const;
    BoxSemanticConfig config_;
    /// Missing chunks, pages, and slots denote top. Property copies share the
    /// sorted root, fixed-width directory chunks, and pages. A write detaches
    /// only the root, affected chunk, and affected page that remain shared.
    std::shared_ptr<BoundPageDirectory> boundPages_;
    bool bottom_ = false;
#ifdef SVF_BOX_STORAGE_TELEMETRY
    std::uint64_t telemetryStateId_ = 0;
#endif
#ifdef SVF_BOX_PAGE_INTERNING
    std::vector<std::size_t> dirtyPages_;
#endif
};

} // namespace SVF::AbstractDomain

#endif // SVF_AE_NUMERICAL_DOMAIN_H
