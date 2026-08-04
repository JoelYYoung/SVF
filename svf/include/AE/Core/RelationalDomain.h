//===- RelationalDomain.h -- Relational numerical domain API ------------===//
//
//                     SVF: Static Value-Flow Analysis
//
// This file defines SVF's relational numerical-domain contract.  Floating-
// point semantics are explicit: callers must select an IEEE format and
// rounding mode instead of silently treating machine values as mathematical
// reals.
//
//===----------------------------------------------------------------------===//

#ifndef SVF_AE_RELATIONALDOMAIN_H
#define SVF_AE_RELATIONALDOMAIN_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SVF
{

using RelationalVariableId = std::uint32_t;

enum class RelationalNumericType
{
    Integer,
    Real,
    Float32,
    Float64
};

enum class RelationalRoundingMode
{
    NearestTiesToEven,
    NearestTiesToAway,
    TowardPositive,
    TowardNegative,
    TowardZero
};

enum class RelationalBinaryOperator
{
    Add,
    Subtract,
    Multiply,
    Divide
};

enum class RelationalPredicate
{
    Equal,
    NotEqual,
    LessThan,
    LessEqual,
    GreaterThan,
    GreaterEqual,

    /// Equality of the IEEE bit patterns.  This deliberately differs from
    /// language-level floating-point equality for NaNs and signed zero.
    BitwiseEqual,
    BitwiseNotEqual
};

enum class RelationalCheckResult
{
    True,
    False,
    Unknown
};

struct RelationalVariable
{
    RelationalVariableId id;
    RelationalNumericType type;
};

struct RelationalExpr
{
    enum class Kind
    {
        Variable,
        IntegerConstant,
        RealConstant,
        FloatConstant,
        Binary
    };

    Kind kind = Kind::IntegerConstant;
    RelationalNumericType type = RelationalNumericType::Integer;
    RelationalVariableId variable = 0;
    std::string literal;
    std::uint64_t floatBits = 0;
    RelationalBinaryOperator binaryOperator = RelationalBinaryOperator::Add;
    RelationalRoundingMode roundingMode =
        RelationalRoundingMode::NearestTiesToEven;
    std::shared_ptr<const RelationalExpr> lhs;
    std::shared_ptr<const RelationalExpr> rhs;

    static RelationalExpr variableExpr(RelationalVariableId id,
                                       RelationalNumericType type);
    static RelationalExpr integerConstant(std::int64_t value);

    /// Create an exact mathematical real.  The literal is accepted in Z3/GMP
    /// rational syntax, for example "3/10", "0.3", or "-17".
    static RelationalExpr realConstant(std::string literal);

    /// Create an IEEE constant from its exact object representation.
    static RelationalExpr float32Constant(std::uint32_t bits);
    static RelationalExpr float64Constant(std::uint64_t bits);

    static RelationalExpr binary(RelationalBinaryOperator op,
                                 RelationalNumericType type,
                                 RelationalExpr lhs,
                                 RelationalExpr rhs,
                                 RelationalRoundingMode rounding =
                                     RelationalRoundingMode::NearestTiesToEven);
};

struct RelationalConstraint
{
    RelationalExpr lhs;
    RelationalPredicate predicate;
    RelationalExpr rhs;
};

/// A relational state denotes a set of valuations over a fixed variable set.
/// Implementations must make assignment a strong update and all lattice
/// operations sound.  A query may return Unknown when the backend times out or
/// only provides a one-sided abstract predicate.
class RelationalDomain
{
public:
    virtual ~RelationalDomain() = default;

    virtual std::unique_ptr<RelationalDomain> clone() const = 0;
    virtual const char* backendName() const = 0;

    virtual void assign(RelationalVariableId target,
                        const RelationalExpr& expression) = 0;
    virtual void assume(const RelationalConstraint& constraint) = 0;
    virtual void forget(RelationalVariableId variable) = 0;

    virtual std::unique_ptr<RelationalDomain>
    join(const RelationalDomain& other) const = 0;
    virtual std::unique_ptr<RelationalDomain>
    meet(const RelationalDomain& other) const = 0;

    /// Return a sound extrapolation containing this state and next.  The usual
    /// fixpoint precondition is that this state is included in next.
    virtual std::unique_ptr<RelationalDomain>
    widening(const RelationalDomain& next) const = 0;

    /// Refine a widened state with a descending successor.  The caller must
    /// establish next <= this state.  Unlike meet, a narrowing is deliberately
    /// allowed to retain some finite bounds so that descending iteration can
    /// be controlled by the backend's finite abstraction.
    virtual std::unique_ptr<RelationalDomain>
    narrowing(const RelationalDomain& next) const = 0;

    /// Return a sound over-approximation that retains lower-bound templates.
    /// "Lower" describes the direction of the retained inequalities; this is
    /// not an under-approximation of reachable states.  A backend may retain
    /// additional information when it has no separate lower-bound projection.
    virtual std::unique_ptr<RelationalDomain>
    lowerBoundApproximation() const = 0;

    virtual RelationalCheckResult isBottom() const = 0;
    virtual RelationalCheckResult
    isSubsetOf(const RelationalDomain& other) const = 0;
    virtual RelationalCheckResult
    entails(const RelationalConstraint& constraint) const = 0;

    virtual std::string toString() const = 0;
};

std::unique_ptr<RelationalDomain>
makeZ3RelationalDomain(const std::vector<RelationalVariable>& variables,
                       unsigned timeoutMilliseconds = 1000);

const char* toString(RelationalCheckResult result);

} // namespace SVF

#endif // SVF_AE_RELATIONALDOMAIN_H
