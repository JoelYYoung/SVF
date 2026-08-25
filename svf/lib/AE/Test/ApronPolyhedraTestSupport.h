//===- ApronPolyhedraTestSupport.h -- APRON differential helpers -*- C++ -*-===//

#ifndef SVF_AE_TEST_APRON_POLYHEDRA_TEST_SUPPORT_H
#define SVF_AE_TEST_APRON_POLYHEDRA_TEST_SUPPORT_H

#include "AE/Core/ConvexPolyhedraDomain.h"

extern "C"
{
#include "ap_abstract0.h"
#include "pk.h"
}

#include <stdexcept>
#include <string>
#include <utility>

namespace SVF::test
{

using namespace AbstractDomain;

class ApronManager
{
public:
    ApronManager() : manager_(pk_manager_alloc(true))
    {
        if (manager_ == nullptr)
            throw std::runtime_error("cannot allocate the NewPolka manager");
    }

    ApronManager(const ApronManager&) = delete;
    ApronManager& operator=(const ApronManager&) = delete;

    ~ApronManager()
    {
        ap_manager_free(manager_);
    }

    ap_manager_t* get() const { return manager_; }

private:
    ap_manager_t* manager_;
};

class ApronValue
{
public:
    ApronValue() = default;
    ApronValue(ap_manager_t* manager, ap_abstract0_t* value)
        : manager_(manager), value_(value)
    {
        if (value_ == nullptr)
            throw std::runtime_error("APRON returned a null abstract value");
    }

    ApronValue(const ApronValue& other)
        : ApronValue(other.manager_,
                     ap_abstract0_copy(other.manager_, other.value_))
    {
    }

    ApronValue(ApronValue&& other) noexcept
        : manager_(std::exchange(other.manager_, nullptr)),
          value_(std::exchange(other.value_, nullptr))
    {
    }

    ApronValue& operator=(ApronValue other) noexcept
    {
        swap(other);
        return *this;
    }

    ~ApronValue()
    {
        if (value_ != nullptr)
            ap_abstract0_free(manager_, value_);
    }

    void swap(ApronValue& other) noexcept
    {
        std::swap(manager_, other.manager_);
        std::swap(value_, other.value_);
    }

    ap_abstract0_t* get() const { return value_; }
    ap_abstract0_t* release()
    {
        manager_ = nullptr;
        return std::exchange(value_, nullptr);
    }

private:
    ap_manager_t* manager_ = nullptr;
    ap_abstract0_t* value_ = nullptr;
};

inline ap_scalar_t* apronScalar(const Rational& value)
{
    return ap_scalar_alloc_set_mpq(
        const_cast<mpq_ptr>(value.value().get_mpq_t()));
}

inline ap_linexpr0_t* apronExpression(const LinearExpression& expression,
                                      const VariableEnvironment& environment,
                                      bool negate = false)
{
    ap_linexpr0_t* result =
        ap_linexpr0_alloc(AP_LINEXPR_DENSE, environment.size());
    const Rational sign(negate ? -1 : 1);
    for (const auto& [variable, coefficient] : expression.terms())
    {
        ap_scalar_t* scalar = apronScalar(coefficient * sign);
        ap_linexpr0_set_coeff_scalar(result, environment.dimensionOf(variable),
                                     scalar);
        ap_scalar_free(scalar);
    }
    ap_scalar_t* constant = apronScalar(expression.constant() * sign);
    ap_linexpr0_set_cst_scalar(result, constant);
    ap_scalar_free(constant);
    return result;
}

inline ap_lincons0_t apronConstraint(const LinearConstraint& constraint,
                                     const VariableEnvironment& environment)
{
    ap_constyp_t type = AP_CONS_EQ;
    bool negate = false;
    switch (constraint.kind())
    {
    case ConstraintKind::Equal:
        type = AP_CONS_EQ;
        break;
    case ConstraintKind::NotEqual:
        type = AP_CONS_DISEQ;
        break;
    case ConstraintKind::GreaterEqual:
        type = AP_CONS_SUPEQ;
        break;
    case ConstraintKind::GreaterThan:
        type = AP_CONS_SUP;
        break;
    case ConstraintKind::LessEqual:
        type = AP_CONS_SUPEQ;
        negate = true;
        break;
    case ConstraintKind::LessThan:
        type = AP_CONS_SUP;
        negate = true;
        break;
    }
    return ap_lincons0_make(type,
                            apronExpression(constraint.expression(),
                                            environment, negate),
                            nullptr);
}

inline ap_lincons0_array_t
apronConstraints(const LinearConstraintSet& constraints,
                 const VariableEnvironment& environment)
{
    ap_lincons0_array_t result = ap_lincons0_array_make(constraints.size());
    for (std::size_t index = 0; index < constraints.size(); ++index)
        result.p[index] = apronConstraint(constraints[index], environment);
    return result;
}

inline void requireRealEnvironment(const VariableEnvironment& environment)
{
    for (const VariableDeclaration& declaration : environment.variables())
        if (declaration.type.kind != NumericKind::Real)
            throw std::invalid_argument(
                "the APRON differential helper currently expects real "
                "dimensions in native environment order");
}

inline ApronValue apronFromConstraints(ap_manager_t* manager,
                                       const VariableEnvironment& environment,
                                       const LinearConstraintSet& constraints)
{
    requireRealEnvironment(environment);
    ap_lincons0_array_t array = apronConstraints(constraints, environment);
    ap_abstract0_t* value = ap_abstract0_of_lincons_array(
        manager, 0, environment.size(), &array);
    ap_lincons0_array_clear(&array);
    return ApronValue(manager, value);
}

inline ApronValue apronFromState(ap_manager_t* manager,
                                 const ConvexPolyhedraState& state)
{
    return apronFromConstraints(manager, state.environment(),
                                state.toConstraints());
}

inline bool apronEqual(ap_manager_t* manager, const ApronValue& lhs,
                       const ApronValue& rhs)
{
    return ap_abstract0_is_eq(manager, lhs.get(), rhs.get());
}

inline bool apronMatches(ap_manager_t* manager,
                         const ConvexPolyhedraState& state,
                         const ApronValue& expected)
{
    const ApronValue actual = apronFromState(manager, state);
    return apronEqual(manager, actual, expected);
}

inline ApronValue apronMeetConstraints(
    ap_manager_t* manager, const ApronValue& state,
    const VariableEnvironment& environment,
    const LinearConstraintSet& constraints)
{
    ap_lincons0_array_t array = apronConstraints(constraints, environment);
    ap_abstract0_t* result = ap_abstract0_meet_lincons_array(
        manager, false, state.get(), &array);
    ap_lincons0_array_clear(&array);
    return ApronValue(manager, result);
}

inline ApronValue apronAssign(ap_manager_t* manager, const ApronValue& state,
                              const VariableEnvironment& environment,
                              Variable target,
                              const LinearExpression& expression)
{
    ap_linexpr0_t* apronExpr = apronExpression(expression, environment);
    ap_abstract0_t* result = ap_abstract0_assign_linexpr(
        manager, false, state.get(), environment.dimensionOf(target),
        apronExpr, nullptr);
    ap_linexpr0_free(apronExpr);
    return ApronValue(manager, result);
}

inline ApronValue apronForget(ap_manager_t* manager, const ApronValue& state,
                              const VariableEnvironment& environment,
                              Variable variable)
{
    ap_dim_t dimension = environment.dimensionOf(variable);
    return ApronValue(manager, ap_abstract0_forget_array(
                                   manager, false, state.get(), &dimension, 1,
                                   false));
}

} // namespace SVF::test

#endif // SVF_AE_TEST_APRON_POLYHEDRA_TEST_SUPPORT_H
