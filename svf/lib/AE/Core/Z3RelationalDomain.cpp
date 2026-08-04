//===- Z3RelationalDomain.cpp -- Exact-formula relational domain --------===//

#include "AE/Core/RelationalDomain.h"

#include "z3++.h"

#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

using namespace SVF;

namespace
{

bool isFloating(RelationalNumericType type)
{
    return type == RelationalNumericType::Float32 ||
           type == RelationalNumericType::Float64;
}

struct Z3RelationalContext
{
    explicit Z3RelationalContext(
        const std::vector<RelationalVariable>& domainVariables,
        unsigned timeout)
        : timeoutMilliseconds(timeout)
    {
        for (const RelationalVariable& variable : domainVariables)
        {
            if (!types.emplace(variable.id, variable.type).second)
                throw std::invalid_argument("duplicate relational variable");

            const std::string name = "rel_" + std::to_string(variable.id);
            switch (variable.type)
            {
            case RelationalNumericType::Integer:
                variables.emplace(variable.id, ctx.int_const(name.c_str()));
                break;
            case RelationalNumericType::Real:
                variables.emplace(variable.id, ctx.real_const(name.c_str()));
                break;
            case RelationalNumericType::Float32:
                variables.emplace(variable.id,
                                  ctx.fpa_const(name.c_str(), 8, 24));
                break;
            case RelationalNumericType::Float64:
                variables.emplace(variable.id,
                                  ctx.fpa_const(name.c_str(), 11, 53));
                break;
            }
        }
    }

    z3::context ctx;
    std::unordered_map<RelationalVariableId, RelationalNumericType> types;
    std::unordered_map<RelationalVariableId, z3::expr> variables;
    unsigned timeoutMilliseconds;
    std::uint64_t nextFreshVariable = 0;
};

class Z3RelationalDomain final : public RelationalDomain
{
public:
    explicit Z3RelationalDomain(std::shared_ptr<Z3RelationalContext> context)
        : context(std::move(context)), formula(this->context->ctx.bool_val(true))
    {
    }

    Z3RelationalDomain(std::shared_ptr<Z3RelationalContext> context,
                       z3::expr formula)
        : context(std::move(context)), formula(std::move(formula))
    {
    }

    std::unique_ptr<RelationalDomain> clone() const override
    {
        return std::make_unique<Z3RelationalDomain>(context, formula);
    }

    const char* backendName() const override
    {
        return "z3-exact-formula";
    }

    void assign(RelationalVariableId target,
                const RelationalExpr& expression) override
    {
        const z3::expr& current = variable(target);
        if (type(target) != expression.type)
            throw std::invalid_argument("assignment type mismatch");

        const std::string oldName =
            "rel_old_" + std::to_string(target) + "_" +
            std::to_string(context->nextFreshVariable++);
        z3::expr old = context->ctx.constant(oldName.c_str(), current.get_sort());

        z3::expr_vector from(context->ctx);
        z3::expr_vector to(context->ctx);
        from.push_back(current);
        to.push_back(old);

        z3::expr oldFormula = formula.substitute(from, to);
        z3::expr rhs = translate(expression).substitute(from, to);
        formula = z3::exists(old, oldFormula && current == rhs);
    }

    void assume(const RelationalConstraint& constraint) override
    {
        formula = formula && translate(constraint);
    }

    void forget(RelationalVariableId id) override
    {
        formula = z3::exists(variable(id), formula);
    }

    std::unique_ptr<RelationalDomain>
    join(const RelationalDomain& other) const override
    {
        const Z3RelationalDomain& rhs = compatible(other);
        return std::make_unique<Z3RelationalDomain>(context,
                                                    formula || rhs.formula);
    }

    std::unique_ptr<RelationalDomain>
    meet(const RelationalDomain& other) const override
    {
        const Z3RelationalDomain& rhs = compatible(other);
        return std::make_unique<Z3RelationalDomain>(context,
                                                    formula && rhs.formula);
    }

    std::unique_ptr<RelationalDomain>
    widening(const RelationalDomain& next) const override
    {
        const Z3RelationalDomain& rhs = compatible(next);
        if (rhs.isSubsetOf(*this) == RelationalCheckResult::True)
            return clone();

        // Formula disjunction is an exact join but is not a widening: an
        // ascending loop sequence can grow forever.  Top is the only generic,
        // backend-independent terminating fallback available without adding a
        // template/polyhedral abstraction layer on top of Z3.
        return std::make_unique<Z3RelationalDomain>(
            context, context->ctx.bool_val(true));
    }

    RelationalCheckResult isBottom() const override
    {
        const RelationalCheckResult satisfiable = checkSatisfiable(formula);
        return satisfiable == RelationalCheckResult::False
                   ? RelationalCheckResult::True
                   : (satisfiable == RelationalCheckResult::True
                          ? RelationalCheckResult::False
                          : RelationalCheckResult::Unknown);
    }

    RelationalCheckResult
    isSubsetOf(const RelationalDomain& other) const override
    {
        const Z3RelationalDomain& rhs = compatible(other);
        return negateSatisfiability(checkSatisfiable(formula && !rhs.formula));
    }

    RelationalCheckResult
    entails(const RelationalConstraint& constraint) const override
    {
        return negateSatisfiability(
            checkSatisfiable(formula && !translate(constraint)));
    }

    std::string toString() const override
    {
        return formula.to_string();
    }

private:
    const z3::expr& variable(RelationalVariableId id) const
    {
        auto found = context->variables.find(id);
        if (found == context->variables.end())
            throw std::invalid_argument("unknown relational variable");
        return found->second;
    }

    RelationalNumericType type(RelationalVariableId id) const
    {
        auto found = context->types.find(id);
        if (found == context->types.end())
            throw std::invalid_argument("unknown relational variable");
        return found->second;
    }

    z3::expr roundingMode(RelationalRoundingMode mode) const
    {
        Z3_ast result = nullptr;
        switch (mode)
        {
        case RelationalRoundingMode::NearestTiesToEven:
            result = Z3_mk_fpa_round_nearest_ties_to_even(context->ctx);
            break;
        case RelationalRoundingMode::NearestTiesToAway:
            result = Z3_mk_fpa_round_nearest_ties_to_away(context->ctx);
            break;
        case RelationalRoundingMode::TowardPositive:
            result = Z3_mk_fpa_round_toward_positive(context->ctx);
            break;
        case RelationalRoundingMode::TowardNegative:
            result = Z3_mk_fpa_round_toward_negative(context->ctx);
            break;
        case RelationalRoundingMode::TowardZero:
            result = Z3_mk_fpa_round_toward_zero(context->ctx);
            break;
        }
        return z3::to_expr(context->ctx, result);
    }

    z3::expr floatingConstant(const RelationalExpr& expression) const
    {
        const unsigned width =
            expression.type == RelationalNumericType::Float32 ? 32 : 64;
        const unsigned exponentBits = width == 32 ? 8 : 11;
        const unsigned significandBits = width == 32 ? 24 : 53;
        const std::string bits = std::to_string(expression.floatBits);
        z3::expr bitVector = context->ctx.bv_val(bits.c_str(), width);
        z3::sort sort = context->ctx.fpa_sort(exponentBits, significandBits);
        return z3::to_expr(context->ctx,
                           Z3_mk_fpa_to_fp_bv(context->ctx, bitVector, sort));
    }

    z3::expr translate(const RelationalExpr& expression) const
    {
        switch (expression.kind)
        {
        case RelationalExpr::Kind::Variable:
            if (type(expression.variable) != expression.type)
                throw std::invalid_argument("variable expression type mismatch");
            return variable(expression.variable);
        case RelationalExpr::Kind::IntegerConstant:
            return context->ctx.int_val(expression.literal.c_str());
        case RelationalExpr::Kind::RealConstant:
            return context->ctx.real_val(expression.literal.c_str());
        case RelationalExpr::Kind::FloatConstant:
            return floatingConstant(expression);
        case RelationalExpr::Kind::Binary:
            break;
        }

        if (!expression.lhs || !expression.rhs)
            throw std::invalid_argument("binary relational expression is incomplete");
        z3::expr lhs = translate(*expression.lhs);
        z3::expr rhs = translate(*expression.rhs);

        if (!isFloating(expression.type))
        {
            switch (expression.binaryOperator)
            {
            case RelationalBinaryOperator::Add:
                return lhs + rhs;
            case RelationalBinaryOperator::Subtract:
                return lhs - rhs;
            case RelationalBinaryOperator::Multiply:
                return lhs * rhs;
            case RelationalBinaryOperator::Divide:
                if (expression.type == RelationalNumericType::Integer)
                    throw std::invalid_argument(
                        "integer division requires an explicit language policy");
                return lhs / rhs;
            }
        }

        const z3::expr rounding = roundingMode(expression.roundingMode);
        Z3_ast result = nullptr;
        switch (expression.binaryOperator)
        {
        case RelationalBinaryOperator::Add:
            result = Z3_mk_fpa_add(context->ctx, rounding, lhs, rhs);
            break;
        case RelationalBinaryOperator::Subtract:
            result = Z3_mk_fpa_sub(context->ctx, rounding, lhs, rhs);
            break;
        case RelationalBinaryOperator::Multiply:
            result = Z3_mk_fpa_mul(context->ctx, rounding, lhs, rhs);
            break;
        case RelationalBinaryOperator::Divide:
            result = Z3_mk_fpa_div(context->ctx, rounding, lhs, rhs);
            break;
        }
        return z3::to_expr(context->ctx, result);
    }

    z3::expr translate(const RelationalConstraint& constraint) const
    {
        if (constraint.lhs.type != constraint.rhs.type)
            throw std::invalid_argument("constraint operand type mismatch");
        z3::expr lhs = translate(constraint.lhs);
        z3::expr rhs = translate(constraint.rhs);
        const bool floating = isFloating(constraint.lhs.type);

        switch (constraint.predicate)
        {
        case RelationalPredicate::Equal:
            return floating
                       ? z3::to_expr(context->ctx,
                                     Z3_mk_fpa_eq(context->ctx, lhs, rhs))
                       : lhs == rhs;
        case RelationalPredicate::NotEqual:
            return floating
                       ? !z3::to_expr(context->ctx,
                                      Z3_mk_fpa_eq(context->ctx, lhs, rhs))
                       : lhs != rhs;
        case RelationalPredicate::LessThan:
            return floating
                       ? z3::to_expr(context->ctx,
                                     Z3_mk_fpa_lt(context->ctx, lhs, rhs))
                       : lhs < rhs;
        case RelationalPredicate::LessEqual:
            return floating
                       ? z3::to_expr(context->ctx,
                                     Z3_mk_fpa_leq(context->ctx, lhs, rhs))
                       : lhs <= rhs;
        case RelationalPredicate::GreaterThan:
            return floating
                       ? z3::to_expr(context->ctx,
                                     Z3_mk_fpa_gt(context->ctx, lhs, rhs))
                       : lhs > rhs;
        case RelationalPredicate::GreaterEqual:
            return floating
                       ? z3::to_expr(context->ctx,
                                     Z3_mk_fpa_geq(context->ctx, lhs, rhs))
                       : lhs >= rhs;
        case RelationalPredicate::BitwiseEqual:
            if (!floating)
                throw std::invalid_argument(
                    "bitwise equality is only defined for IEEE floats");
            return lhs == rhs;
        case RelationalPredicate::BitwiseNotEqual:
            if (!floating)
                throw std::invalid_argument(
                    "bitwise inequality is only defined for IEEE floats");
            return lhs != rhs;
        }
        throw std::invalid_argument("unknown relational predicate");
    }

    RelationalCheckResult checkSatisfiable(const z3::expr& expression) const
    {
        z3::solver solver(context->ctx);
        z3::params parameters(context->ctx);
        parameters.set("timeout", context->timeoutMilliseconds);
        solver.set(parameters);
        solver.add(expression);
        switch (solver.check())
        {
        case z3::sat:
            return RelationalCheckResult::True;
        case z3::unsat:
            return RelationalCheckResult::False;
        case z3::unknown:
            return RelationalCheckResult::Unknown;
        }
        return RelationalCheckResult::Unknown;
    }

    static RelationalCheckResult
    negateSatisfiability(RelationalCheckResult satisfiable)
    {
        if (satisfiable == RelationalCheckResult::True)
            return RelationalCheckResult::False;
        if (satisfiable == RelationalCheckResult::False)
            return RelationalCheckResult::True;
        return RelationalCheckResult::Unknown;
    }

    const Z3RelationalDomain& compatible(const RelationalDomain& other) const
    {
        const auto* rhs = dynamic_cast<const Z3RelationalDomain*>(&other);
        if (!rhs || rhs->context.get() != context.get())
            throw std::invalid_argument(
                "relational operands must be clones from the same Z3 state");
        return *rhs;
    }

    std::shared_ptr<Z3RelationalContext> context;
    z3::expr formula;
};

} // namespace

std::unique_ptr<RelationalDomain> SVF::makeZ3RelationalDomain(
    const std::vector<RelationalVariable>& variables,
    unsigned timeoutMilliseconds)
{
    return std::make_unique<Z3RelationalDomain>(
        std::make_shared<Z3RelationalContext>(variables,
                                              timeoutMilliseconds));
}
