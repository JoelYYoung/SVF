//===- ApronRelationalDomain.cpp -- APRON polyhedra relational domain ----===//

#include "AE/Core/RelationalDomain.h"

#include <stdexcept>

#ifndef SVF_ENABLE_APRON

std::unique_ptr<SVF::RelationalDomain> SVF::makeApronRelationalDomain(
    const std::vector<RelationalVariable>&)
{
    throw std::logic_error(
        "SVF was built without APRON; configure with -DSVF_ENABLE_APRON=ON");
}

#else

#include "ap_abstract0.h"
#include "ap_global0.h"
#include "pk.h"

#include <cmath>
#include <cctype>
#include <cstring>
#include <gmp.h>
#include <sstream>
#include <unordered_map>
#include <utility>

using namespace SVF;

namespace
{

struct ApronRelationalContext
{
    explicit ApronRelationalContext(
        const std::vector<RelationalVariable>& variables)
    {
        for (const RelationalVariable& variable : variables)
        {
            if (!types.emplace(variable.id, variable.type).second)
                throw std::invalid_argument("duplicate relational variable");
        }

        for (const RelationalVariable& variable : variables)
        {
            if (variable.type == RelationalNumericType::Integer)
                dimensions.emplace(variable.id, integerDimensions++);
        }
        for (const RelationalVariable& variable : variables)
        {
            if (variable.type != RelationalNumericType::Integer)
                dimensions.emplace(variable.id,
                                   integerDimensions + realDimensions++);
        }

        // NewPolka is APRON's built-in convex-polyhedra domain.  It uses exact
        // rational coefficients internally; tree expressions with SINGLE or
        // DOUBLE operations are soundly linearized before transfer.
        manager = pk_manager_alloc(true);
        if (!manager)
            throw std::runtime_error("failed to allocate APRON manager");
    }

    ~ApronRelationalContext()
    {
        ap_manager_free(manager);
    }

    ap_manager_t* manager = nullptr;
    std::unordered_map<RelationalVariableId, RelationalNumericType> types;
    std::unordered_map<RelationalVariableId, ap_dim_t> dimensions;
    std::size_t integerDimensions = 0;
    std::size_t realDimensions = 0;
};

class ApronRelationalDomain final : public RelationalDomain
{
public:
    explicit ApronRelationalDomain(
        std::shared_ptr<ApronRelationalContext> context)
        : context(std::move(context))
    {
        value = ap_abstract0_top(this->context->manager,
                                 this->context->integerDimensions,
                                 this->context->realDimensions);
    }

    ApronRelationalDomain(std::shared_ptr<ApronRelationalContext> context,
                          ap_abstract0_t* value)
        : context(std::move(context)), value(value)
    {
        if (!this->value)
            throw std::runtime_error("APRON returned a null abstract value");
    }

    ~ApronRelationalDomain() override
    {
        ap_abstract0_free(context->manager, value);
    }

    std::unique_ptr<RelationalDomain> clone() const override
    {
        return std::make_unique<ApronRelationalDomain>(
            context, ap_abstract0_copy(context->manager, value));
    }

    const char* backendName() const override
    {
        return "apron-newpolka";
    }

    void assign(RelationalVariableId target,
                const RelationalExpr& expression) override
    {
        if (type(target) != expression.type)
            throw std::invalid_argument("assignment type mismatch");
        ap_texpr0_t* rhs = translate(expression);
        ap_abstract0_t* result = ap_abstract0_assign_texpr(
            context->manager, false, value, dimension(target), rhs, nullptr);
        ap_texpr0_free(rhs);
        replace(result);
    }

    void assume(const RelationalConstraint& constraint) override
    {
        ap_tcons0_t converted = translate(constraint);
        ap_tcons0_array_t constraints = ap_tcons0_array_make(1);
        constraints.p[0] = converted;
        ap_abstract0_t* result = ap_abstract0_meet_tcons_array(
            context->manager, false, value, &constraints);
        ap_tcons0_array_clear(&constraints);
        replace(result);
    }

    void forget(RelationalVariableId id) override
    {
        ap_dim_t dim = dimension(id);
        replace(ap_abstract0_forget_array(context->manager, false, value, &dim,
                                          1, false));
    }

    std::unique_ptr<RelationalDomain>
    join(const RelationalDomain& other) const override
    {
        const ApronRelationalDomain& rhs = compatible(other);
        return std::make_unique<ApronRelationalDomain>(
            context, ap_abstract0_join(context->manager, false, value,
                                       rhs.value));
    }

    std::unique_ptr<RelationalDomain>
    meet(const RelationalDomain& other) const override
    {
        const ApronRelationalDomain& rhs = compatible(other);
        return std::make_unique<ApronRelationalDomain>(
            context, ap_abstract0_meet(context->manager, false, value,
                                       rhs.value));
    }

    std::unique_ptr<RelationalDomain>
    widening(const RelationalDomain& next) const override
    {
        const ApronRelationalDomain& rhs = compatible(next);
        return std::make_unique<ApronRelationalDomain>(
            context,
            ap_abstract0_widening(context->manager, value, rhs.value));
    }

    RelationalCheckResult isBottom() const override
    {
        const bool result = ap_abstract0_is_bottom(context->manager, value);
        return abstractPredicate(result);
    }

    RelationalCheckResult
    isSubsetOf(const RelationalDomain& other) const override
    {
        const ApronRelationalDomain& rhs = compatible(other);
        const bool result =
            ap_abstract0_is_leq(context->manager, value, rhs.value);
        return abstractPredicate(result);
    }

    RelationalCheckResult
    entails(const RelationalConstraint& constraint) const override
    {
        ap_tcons0_t converted = translate(constraint);
        const bool result =
            ap_abstract0_sat_tcons(context->manager, value, &converted);
        ap_tcons0_clear(&converted);
        if (result)
            return RelationalCheckResult::True;

        // A convex domain may not represent a disequality directly even when
        // it can prove one.  Entailment of c is equivalently emptiness of the
        // state intersected with !c; a bottom result remains a sound proof
        // even when the meet itself is an over-approximation.
        RelationalConstraint negated = constraint;
        negated.predicate = negate(constraint.predicate);
        ap_tcons0_t negatedConstraint = translate(negated);
        ap_tcons0_array_t constraints = ap_tcons0_array_make(1);
        constraints.p[0] = negatedConstraint;
        ap_abstract0_t* violating = ap_abstract0_meet_tcons_array(
            context->manager, false, value, &constraints);
        ap_tcons0_array_clear(&constraints);
        if (!violating)
            return RelationalCheckResult::Unknown;
        const bool empty =
            ap_abstract0_is_bottom(context->manager, violating);
        const RelationalCheckResult emptiness = abstractPredicate(empty);
        ap_abstract0_free(context->manager, violating);
        if (emptiness == RelationalCheckResult::True)
            return RelationalCheckResult::True;
        return RelationalCheckResult::Unknown;
    }

    std::string toString() const override
    {
        std::ostringstream out;
        out << context->manager->library << " " << context->manager->version
            << " (" << context->integerDimensions << " integer, "
            << context->realDimensions << " real/IEEE dimensions)";
        return out.str();
    }

private:
    RelationalNumericType type(RelationalVariableId id) const
    {
        auto found = context->types.find(id);
        if (found == context->types.end())
            throw std::invalid_argument("unknown relational variable");
        return found->second;
    }

    ap_dim_t dimension(RelationalVariableId id) const
    {
        auto found = context->dimensions.find(id);
        if (found == context->dimensions.end())
            throw std::invalid_argument("unknown relational variable");
        return found->second;
    }

    static ap_texpr_rtype_t roundingType(RelationalNumericType type)
    {
        switch (type)
        {
        case RelationalNumericType::Integer:
            return AP_RTYPE_INT;
        case RelationalNumericType::Real:
            return AP_RTYPE_REAL;
        case RelationalNumericType::Float32:
            return AP_RTYPE_SINGLE;
        case RelationalNumericType::Float64:
            return AP_RTYPE_DOUBLE;
        }
        return AP_RTYPE_REAL;
    }

    static ap_texpr_rdir_t roundingDirection(RelationalRoundingMode mode)
    {
        switch (mode)
        {
        case RelationalRoundingMode::NearestTiesToEven:
            return AP_RDIR_NEAREST;
        case RelationalRoundingMode::TowardPositive:
            return AP_RDIR_UP;
        case RelationalRoundingMode::TowardNegative:
            return AP_RDIR_DOWN;
        case RelationalRoundingMode::TowardZero:
            return AP_RDIR_ZERO;
        case RelationalRoundingMode::NearestTiesToAway:
            throw std::invalid_argument(
                "APRON has no nearest-ties-away rounding selector");
        }
        return AP_RDIR_NEAREST;
    }

    static RelationalPredicate negate(RelationalPredicate predicate)
    {
        switch (predicate)
        {
        case RelationalPredicate::Equal:
            return RelationalPredicate::NotEqual;
        case RelationalPredicate::NotEqual:
            return RelationalPredicate::Equal;
        case RelationalPredicate::LessThan:
            return RelationalPredicate::GreaterEqual;
        case RelationalPredicate::LessEqual:
            return RelationalPredicate::GreaterThan;
        case RelationalPredicate::GreaterThan:
            return RelationalPredicate::LessEqual;
        case RelationalPredicate::GreaterEqual:
            return RelationalPredicate::LessThan;
        case RelationalPredicate::BitwiseEqual:
            return RelationalPredicate::BitwiseNotEqual;
        case RelationalPredicate::BitwiseNotEqual:
            return RelationalPredicate::BitwiseEqual;
        }
        throw std::invalid_argument("unknown relational predicate");
    }

    ap_texpr0_t* rationalConstant(const std::string& literal) const
    {
        std::string rationalLiteral = literal;
        const std::size_t exponentPosition = literal.find_first_of("eE");
        const std::size_t decimalPosition = literal.find('.');
        if (decimalPosition != std::string::npos ||
                exponentPosition != std::string::npos)
        {
            const std::string mantissa = literal.substr(0, exponentPosition);
            const long exponent = exponentPosition == std::string::npos
                                      ? 0
                                      : std::stol(literal.substr(
                                            exponentPosition + 1));
            bool negative = false;
            std::size_t start = 0;
            if (!mantissa.empty() &&
                    (mantissa.front() == '-' || mantissa.front() == '+'))
            {
                negative = mantissa.front() == '-';
                start = 1;
            }
            const std::size_t dot = mantissa.find('.', start);
            const std::size_t fractionalDigits =
                dot == std::string::npos ? 0 : mantissa.size() - dot - 1;
            std::string digits;
            for (std::size_t index = start; index < mantissa.size(); ++index)
            {
                if (mantissa[index] == '.')
                    continue;
                if (!std::isdigit(static_cast<unsigned char>(mantissa[index])))
                    throw std::invalid_argument("invalid APRON decimal literal");
                digits.push_back(mantissa[index]);
            }
            if (digits.empty())
                throw std::invalid_argument("invalid APRON decimal literal");

            const long scale = static_cast<long>(fractionalDigits) - exponent;
            if (scale <= 0)
                digits.append(static_cast<std::size_t>(-scale), '0');
            std::string numerator = (negative ? "-" : "") + digits;
            rationalLiteral = numerator;
            if (scale > 0)
                rationalLiteral += "/1" +
                                   std::string(static_cast<std::size_t>(scale),
                                               '0');
        }

        mpq_t rational;
        mpq_init(rational);
        if (mpq_set_str(rational, rationalLiteral.c_str(), 10) != 0)
        {
            mpq_clear(rational);
            throw std::invalid_argument("invalid APRON rational literal");
        }
        mpq_canonicalize(rational);
        ap_texpr0_t* result = ap_texpr0_cst_scalar_mpq(rational);
        mpq_clear(rational);
        return result;
    }

    ap_texpr0_t* floatingConstant(const RelationalExpr& expression) const
    {
        double value = 0.0;
        if (expression.type == RelationalNumericType::Float32)
        {
            const std::uint32_t bits =
                static_cast<std::uint32_t>(expression.floatBits);
            float single = 0.0F;
            std::memcpy(&single, &bits, sizeof(single));
            value = single;
        }
        else
        {
            const std::uint64_t bits = expression.floatBits;
            std::memcpy(&value, &bits, sizeof(value));
        }

        // APRON numerical dimensions denote finite mathematical numbers.  Its
        // tree-expression linearizer models rounding error, but NewPolka does
        // not encode IEEE NaN or infinities as values.
        if (!std::isfinite(value))
            throw std::invalid_argument(
                "APRON NewPolka cannot represent NaN or infinity");
        return ap_texpr0_cst_scalar_double(value);
    }

    ap_texpr0_t* translate(const RelationalExpr& expression) const
    {
        switch (expression.kind)
        {
        case RelationalExpr::Kind::Variable:
            if (type(expression.variable) != expression.type)
                throw std::invalid_argument("variable expression type mismatch");
            return ap_texpr0_dim(dimension(expression.variable));
        case RelationalExpr::Kind::IntegerConstant:
            return rationalConstant(expression.literal);
        case RelationalExpr::Kind::RealConstant:
            return rationalConstant(expression.literal);
        case RelationalExpr::Kind::FloatConstant:
            return floatingConstant(expression);
        case RelationalExpr::Kind::Binary:
            break;
        }

        if (!expression.lhs || !expression.rhs)
            throw std::invalid_argument("binary relational expression is incomplete");
        ap_texpr_op_t operation = AP_TEXPR_ADD;
        switch (expression.binaryOperator)
        {
        case RelationalBinaryOperator::Add:
            operation = AP_TEXPR_ADD;
            break;
        case RelationalBinaryOperator::Subtract:
            operation = AP_TEXPR_SUB;
            break;
        case RelationalBinaryOperator::Multiply:
            operation = AP_TEXPR_MUL;
            break;
        case RelationalBinaryOperator::Divide:
            operation = AP_TEXPR_DIV;
            break;
        }
        return ap_texpr0_binop(operation, translate(*expression.lhs),
                               translate(*expression.rhs),
                               roundingType(expression.type),
                               roundingDirection(expression.roundingMode));
    }

    ap_tcons0_t translate(const RelationalConstraint& constraint) const
    {
        if (constraint.lhs.type != constraint.rhs.type)
            throw std::invalid_argument("constraint operand type mismatch");
        if (constraint.predicate == RelationalPredicate::BitwiseEqual ||
                constraint.predicate == RelationalPredicate::BitwiseNotEqual)
            throw std::invalid_argument(
                "APRON cannot distinguish IEEE bit patterns");

        bool reverse = false;
        ap_constyp_t predicate = AP_CONS_EQ;
        switch (constraint.predicate)
        {
        case RelationalPredicate::Equal:
            predicate = AP_CONS_EQ;
            break;
        case RelationalPredicate::NotEqual:
            predicate = AP_CONS_DISEQ;
            break;
        case RelationalPredicate::LessThan:
            predicate = AP_CONS_SUP;
            reverse = true;
            break;
        case RelationalPredicate::LessEqual:
            predicate = AP_CONS_SUPEQ;
            reverse = true;
            break;
        case RelationalPredicate::GreaterThan:
            predicate = AP_CONS_SUP;
            break;
        case RelationalPredicate::GreaterEqual:
            predicate = AP_CONS_SUPEQ;
            break;
        case RelationalPredicate::BitwiseEqual:
        case RelationalPredicate::BitwiseNotEqual:
            break;
        }

        // IEEE comparison between finite represented values is an ordering of
        // their mathematical values; it must not introduce an extra rounded
        // subtraction.  APRON uses REAL here even when the dimensions were
        // populated by SINGLE/DOUBLE assignments.
        ap_texpr0_t* difference = ap_texpr0_binop(
            AP_TEXPR_SUB,
            translate(reverse ? constraint.rhs : constraint.lhs),
            translate(reverse ? constraint.lhs : constraint.rhs),
            AP_RTYPE_REAL, AP_RDIR_NEAREST);
        return ap_tcons0_make(predicate, difference, nullptr);
    }

    RelationalCheckResult abstractPredicate(bool result) const
    {
        if (result)
            return RelationalCheckResult::True;
        return ap_manager_get_flag_exact(context->manager)
                   ? RelationalCheckResult::False
                   : RelationalCheckResult::Unknown;
    }

    void replace(ap_abstract0_t* replacement)
    {
        if (!replacement)
            throw std::runtime_error("APRON returned a null abstract value");
        ap_abstract0_free(context->manager, value);
        value = replacement;
    }

    const ApronRelationalDomain&
    compatible(const RelationalDomain& other) const
    {
        const auto* rhs = dynamic_cast<const ApronRelationalDomain*>(&other);
        if (!rhs || rhs->context.get() != context.get())
            throw std::invalid_argument(
                "relational operands must be clones from the same APRON state");
        return *rhs;
    }

    std::shared_ptr<ApronRelationalContext> context;
    ap_abstract0_t* value = nullptr;
};

} // namespace

std::unique_ptr<RelationalDomain> SVF::makeApronRelationalDomain(
    const std::vector<RelationalVariable>& variables)
{
    return std::make_unique<ApronRelationalDomain>(
        std::make_shared<ApronRelationalContext>(variables));
}

#endif // SVF_ENABLE_APRON
