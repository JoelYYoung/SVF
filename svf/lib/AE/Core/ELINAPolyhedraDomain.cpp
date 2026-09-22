//===- ELINAPolyhedraDomain.cpp -- ELINA Polyhedra adapter --------===//

#include "AE/Core/ELINAPolyhedraDomain.h"

extern "C"
{
#include "elina_abstract0.h"
#include "opt_pk.h"
}

#include <algorithm>
#include <cfenv>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SVF::AbstractDomain
{

namespace
{

class ELINARoundingScope
{
public:
    ELINARoundingScope() : previous_(std::fegetround())
    {
        if (std::fesetround(FE_UPWARD) != 0)
            throw std::runtime_error("failed to select ELINA upward rounding");
    }
    ~ELINARoundingScope()
    {
        (void)std::fesetround(previous_);
    }

private:
    int previous_;
};

void resetResult(elina_manager_t* manager)
{
    elina_manager_clear_exclog(manager);
    manager->result.exn = ELINA_EXC_NONE;
    manager->result.flag_exact = false;
    manager->result.flag_best = false;
}

void setCoefficient(elina_coeff_t* target, const Rational& value)
{
    mpq_t copy;
    mpq_init(copy);
    mpq_set(copy, value.value().get_mpq_t());
    elina_coeff_set_scalar_mpq(target, copy);
    mpq_clear(copy);
}

Rational scalarValue(elina_scalar_t* scalar)
{
    if (elina_scalar_infty(scalar) != 0)
        throw std::invalid_argument("infinite ELINA scalar is not rational");
    mpq_t value;
    mpq_init(value);
    (void)elina_mpq_set_scalar(value, scalar, GMP_RNDN);
    mpq_class copied;
    mpq_set(copied.get_mpq_t(), value);
    const Rational result = Rational::fromRaw(copied);
    mpq_clear(value);
    return result;
}

Rational coefficientValue(elina_coeff_t* coefficient)
{
    elina_coeff_reduce(coefficient);
    if (coefficient->discr != ELINA_COEFF_SCALAR)
        throw std::runtime_error(
            "ELINA returned a non-scalar linear coefficient");
    return scalarValue(coefficient->val.scalar);
}

bool variableOrder(Variable lhs, Variable rhs)
{
    const bool lhsInteger = lhs.type().kind == NumericKind::Integer;
    const bool rhsInteger = rhs.type().kind == NumericKind::Integer;
    if (lhsInteger != rhsInteger)
        return lhsInteger;
    return lhs < rhs;
}

std::vector<Variable> orderedUnique(std::vector<Variable> variables)
{
    std::sort(variables.begin(), variables.end(), variableOrder);
    variables.erase(std::unique(variables.begin(), variables.end()),
                    variables.end());
    return variables;
}

std::vector<Variable> expressionVariables(const LinearExpression& expression)
{
    std::vector<Variable> result;
    result.reserve(expression.terms().size());
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)coefficient;
        result.push_back(variable);
    }
    return result;
}

std::size_t integerDimensions(const std::vector<Variable>& variables)
{
    return static_cast<std::size_t>(std::count_if(
        variables.begin(), variables.end(), [](Variable variable) {
            return variable.type().kind == NumericKind::Integer;
        }));
}

bool hasNonIntegerVariable(const std::vector<Variable>& variables)
{
    return std::any_of(variables.begin(), variables.end(),
                       [](Variable variable) {
                           return variable.type().kind != NumericKind::Integer;
                       });
}

std::size_t dimensionOf(const std::vector<Variable>& variables,
                        Variable variable)
{
    const auto found = std::find(variables.begin(), variables.end(), variable);
    if (found == variables.end())
        throw std::logic_error("ELINA expression variable missing from layout");
    return static_cast<std::size_t>(found - variables.begin());
}

elina_linexpr0_t* makeExpression(const LinearExpression& expression,
                                 const std::vector<Variable>& variables)
{
    elina_linexpr0_t* result =
        elina_linexpr0_alloc(ELINA_LINEXPR_DENSE, variables.size());
    setCoefficient(elina_linexpr0_cstref(result), expression.constant());
    for (const auto& [variable, coefficient] : expression.terms())
    {
        setCoefficient(elina_linexpr0_coeffref(
                           result, static_cast<elina_dim_t>(
                                       dimensionOf(variables, variable))),
                       coefficient);
    }
    return result;
}

LinearExpression integralizedConstraintExpression(
    const LinearExpression& expression)
{
    mpz_class scale = expression.constant().value().get_den();
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)variable;
        const mpz_class denominator = coefficient.value().get_den();
        mpz_lcm(scale.get_mpz_t(), scale.get_mpz_t(), denominator.get_mpz_t());
    }
    return expression * Rational(Integer(scale.get_str()));
}

elina_lincons0_t makeConstraint(const LinearConstraint& input,
                                const std::vector<Variable>& variables,
                                bool& approximated)
{
    LinearConstraint constraint = input;
    if (input.kind() == ConstraintKind::LessThan ||
        input.kind() == ConstraintKind::GreaterThan)
    {
        const auto rewritten = rewriteStrictIntegerConstraint(input);
        if (rewritten)
            constraint = *rewritten;
        else
        {
            approximated = true;
            constraint = LinearConstraint(
                input.expression(), input.kind() == ConstraintKind::LessThan
                                        ? ConstraintKind::LessEqual
                                        : ConstraintKind::GreaterEqual);
        }
    }

    // f524156d's optimized Polyhedra carrier stores integer rows. Supplying
    // rational coefficients directly can truncate them (for example x=1/2
    // becoming x=1), so clear all denominators by one positive scale first.
    LinearExpression expression =
        integralizedConstraintExpression(constraint.expression());
    elina_constyp_t type = ELINA_CONS_SUPEQ;
    switch (constraint.kind())
    {
    case ConstraintKind::Equal:
        type = ELINA_CONS_EQ;
        break;
    case ConstraintKind::NotEqual:
        type = ELINA_CONS_DISEQ;
        break;
    case ConstraintKind::LessEqual:
    case ConstraintKind::LessThan:
        expression = -expression;
        type = ELINA_CONS_SUPEQ;
        break;
    case ConstraintKind::GreaterEqual:
    case ConstraintKind::GreaterThan:
        type = ELINA_CONS_SUPEQ;
        break;
    }
    return elina_lincons0_make(type, makeExpression(expression, variables),
                               nullptr);
}

LinearExpression expressionFromELINA(elina_linexpr0_t* expression,
                                     const std::vector<Variable>& variables)
{
    LinearExpression result(
        coefficientValue(elina_linexpr0_cstref(expression)));
    std::size_t index = 0;
    elina_dim_t dimension = 0;
    elina_coeff_t* coefficient = nullptr;
    elina_linexpr0_ForeachLinterm(expression, index, dimension, coefficient)
    {
        if (dimension >= variables.size())
            throw std::runtime_error("ELINA exported an invalid dimension");
        const Rational value = coefficientValue(coefficient);
        if (!value.isZero())
            result.setCoefficient(variables[dimension], value);
    }
    return result;
}

LinearConstraint constraintFromELINA(elina_lincons0_t& constraint,
                                     const std::vector<Variable>& variables)
{
    LinearExpression expression =
        expressionFromELINA(constraint.linexpr0, variables);
    switch (constraint.constyp)
    {
    case ELINA_CONS_EQ:
        return LinearConstraint(std::move(expression), ConstraintKind::Equal);
    case ELINA_CONS_SUPEQ:
        return LinearConstraint(std::move(expression),
                                ConstraintKind::GreaterEqual);
    case ELINA_CONS_SUP:
        return LinearConstraint(std::move(expression),
                                ConstraintKind::GreaterThan);
    case ELINA_CONS_DISEQ:
        return LinearConstraint(std::move(expression),
                                ConstraintKind::NotEqual);
    case ELINA_CONS_EQMOD:
        throw std::runtime_error("ELINA EQMOD export is unsupported");
    }
    throw std::runtime_error("ELINA exported an unknown constraint kind");
}

Interval intervalFromELINA(elina_interval_t* interval)
{
    if (elina_interval_is_bottom(interval))
        return Interval::bottom();
    const int lowerInfinity = elina_scalar_infty(interval->inf);
    const int upperInfinity = elina_scalar_infty(interval->sup);
    const Bound lower = lowerInfinity < 0
                            ? Bound::minusInfinity()
                            : Bound::finite(scalarValue(interval->inf));
    const Bound upper = upperInfinity > 0
                            ? Bound::plusInfinity()
                            : Bound::finite(scalarValue(interval->sup));
    return Interval(lower, upper);
}

struct ELINAStatus
{
    bool ok = false;
    bool exact = false;
    bool best = false;
    std::string exception;
};

} // namespace

class ELINAPolyhedraDomain::Impl
{
public:
    explicit Impl(bool bottom)
    {
        const int previous = std::fegetround();
        manager = opt_pk_manager_alloc(false);
        (void)std::fesetround(previous);
        if (!manager)
            throw std::runtime_error(
                "ELINA Polyhedra manager allocation failed");
        ELINARoundingScope rounding;
        resetResult(manager);
        state = bottom ? elina_abstract0_bottom(manager, 0, 0)
                       : elina_abstract0_top(manager, 0, 0);
        if (!state || manager->result.exn != ELINA_EXC_NONE)
            throw std::runtime_error("ELINA Polyhedra state allocation failed");
    }

    Impl(const Impl& other) : Impl(false)
    {
        ELINARoundingScope rounding;
        elina_abstract0_free(manager, state);
        variables = other.variables;
        resetResult(manager);
        state = elina_abstract0_copy(manager, other.state);
        if (!state || manager->result.exn != ELINA_EXC_NONE)
            throw std::runtime_error("ELINA Polyhedra copy failed");
    }

    ~Impl()
    {
        const int previous = std::fegetround();
        (void)std::fesetround(FE_UPWARD);
        if (state)
            elina_abstract0_free(manager, state);
        elina_manager_free(manager);
        (void)std::fesetround(previous);
    }

    ELINAStatus adopt(elina_abstract0_t* next)
    {
        ELINAStatus status;
        status.exact = manager->result.flag_exact;
        status.best = manager->result.flag_best;
        status.ok = next != nullptr && manager->result.exn == ELINA_EXC_NONE;
        if (manager->result.exn != ELINA_EXC_NONE)
            status.exception = elina_name_of_exception[manager->result.exn];
        if (status.ok)
        {
            elina_abstract0_free(manager, state);
            state = next;
        }
        else if (next)
            elina_abstract0_free(manager, next);
        return status;
    }

    elina_manager_t* manager = nullptr;
    elina_abstract0_t* state = nullptr;
    std::vector<Variable> variables;
};

ELINAPolyhedraDomain::ELINAPolyhedraDomain(ELINAPolyhedraConfig config,
                                           bool bottom)
    : config_(std::move(config)), impl_(std::make_unique<Impl>(bottom))
{
}

ELINAPolyhedraDomain ELINAPolyhedraDomain::top(
    const ELINAPolyhedraConfig& config)
{
    return ELINAPolyhedraDomain(config, false);
}

ELINAPolyhedraDomain ELINAPolyhedraDomain::bottom(
    const ELINAPolyhedraConfig& config)
{
    return ELINAPolyhedraDomain(config, true);
}

ELINAPolyhedraDomain::ELINAPolyhedraDomain(const ELINAPolyhedraDomain& other)
    : NumericalDomain(other), config_(other.config_),
      impl_(std::make_unique<Impl>(*other.impl_)),
      fallback_(other.fallback_
                    ? std::make_unique<ConvexPolyhedraDomain>(*other.fallback_)
                    : nullptr)
{
}

ELINAPolyhedraDomain::ELINAPolyhedraDomain(
    ELINAPolyhedraDomain&& other) noexcept = default;

ELINAPolyhedraDomain& ELINAPolyhedraDomain::operator=(
    const ELINAPolyhedraDomain& other)
{
    if (this == &other)
        return *this;
    NumericalDomain::operator=(other);
    config_ = other.config_;
    impl_ = std::make_unique<Impl>(*other.impl_);
    fallback_ = other.fallback_
                    ? std::make_unique<ConvexPolyhedraDomain>(*other.fallback_)
                    : nullptr;
    return *this;
}

ELINAPolyhedraDomain& ELINAPolyhedraDomain::operator=(
    ELINAPolyhedraDomain&& other) noexcept = default;

ELINAPolyhedraDomain::~ELINAPolyhedraDomain() = default;

std::unique_ptr<AbstractDomain> ELINAPolyhedraDomain::clone() const
{
    return std::make_unique<ELINAPolyhedraDomain>(*this);
}

void ELINAPolyhedraDomain::report(OperationKind operation,
                                  ApproximationKind approximation,
                                  std::string reason, bool best) const
{
    recordOperation(operation, approximation, best, reason);
    if (config_.diagnostics && approximation != ApproximationKind::Exact)
        config_.diagnostics->report(
            {operation, approximation, std::move(reason)});
}

void ELINAPolyhedraDomain::ensureVariables(
    const std::vector<Variable>& requested)
{
    if (hasNonIntegerVariable(requested))
        throw std::invalid_argument(
            "fixed ELINA Polyhedra does not preserve non-integer dimensions");
    for (Variable variable : orderedUnique(requested))
    {
        if (std::find(impl_->variables.begin(), impl_->variables.end(),
                      variable) != impl_->variables.end())
            continue;
        const auto position =
            std::lower_bound(impl_->variables.begin(), impl_->variables.end(),
                             variable, variableOrder);
        const std::size_t index =
            static_cast<std::size_t>(position - impl_->variables.begin());
        elina_dimchange_t change;
        const bool integer = variable.type().kind == NumericKind::Integer;
        elina_dimchange_init(&change, integer ? 1 : 0, integer ? 0 : 1);
        change.dim[0] = static_cast<elina_dim_t>(index);
        ELINARoundingScope rounding;
        resetResult(impl_->manager);
        elina_abstract0_t* next = elina_abstract0_add_dimensions(
            impl_->manager, false, impl_->state, &change, false);
        elina_dimchange_clear(&change);
        const ELINAStatus status = impl_->adopt(next);
        if (!status.ok)
            throw std::runtime_error("ELINA add-dimension failed: " +
                                     status.exception);
        impl_->variables.insert(position, variable);
    }
}

void ELINAPolyhedraDomain::removeVariables(const std::vector<Variable>& removed)
{
    std::vector<std::size_t> dimensions;
    for (Variable variable : removed)
    {
        const auto found = std::find(impl_->variables.begin(),
                                     impl_->variables.end(), variable);
        if (found != impl_->variables.end())
            dimensions.push_back(
                static_cast<std::size_t>(found - impl_->variables.begin()));
    }
    if (dimensions.empty())
        return;
    std::sort(dimensions.begin(), dimensions.end());
    dimensions.erase(std::unique(dimensions.begin(), dimensions.end()),
                     dimensions.end());
    const std::size_t integers = integerDimensions(impl_->variables);
    const std::size_t removedIntegers = static_cast<std::size_t>(std::count_if(
        dimensions.begin(), dimensions.end(),
        [&](std::size_t dimension) { return dimension < integers; }));
    elina_dimchange_t change;
    elina_dimchange_init(&change, removedIntegers,
                         dimensions.size() - removedIntegers);
    for (std::size_t index = 0; index < dimensions.size(); ++index)
        change.dim[index] = static_cast<elina_dim_t>(dimensions[index]);
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_remove_dimensions(
        impl_->manager, false, impl_->state, &change);
    elina_dimchange_clear(&change);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
        throw std::runtime_error("ELINA remove-dimension failed: " +
                                 status.exception);
    for (auto it = dimensions.rbegin(); it != dimensions.rend(); ++it)
        impl_->variables.erase(impl_->variables.begin() +
                               static_cast<std::ptrdiff_t>(*it));
}

ConvexPolyhedraDomain ELINAPolyhedraDomain::nativeSnapshot() const
{
    if (fallback_)
        return *fallback_;
    ConvexPolyhedraConfig config;
    config.diagnostics = config_.diagnostics;
    config.integerTightening = config_.integerTightening;
    if (isBottom())
        return ConvexPolyhedraDomain::bottom(config);
    return ConvexPolyhedraDomain::fromConstraints(toConstraints(), config);
}

void ELINAPolyhedraDomain::replaceFromNative(
    const ConvexPolyhedraDomain& native, OperationKind operation,
    const std::string& fallbackOperation)
{
    const LinearConstraintSet constraints = native.toConstraints();
    std::vector<Variable> variables = native.supportVariables();
    for (const LinearConstraint& constraint : constraints)
    {
        const auto used = expressionVariables(constraint.expression());
        variables.insert(variables.end(), used.begin(), used.end());
    }
    variables = orderedUnique(std::move(variables));
    if (hasNonIntegerVariable(variables))
    {
        fallback_ = std::make_unique<ConvexPolyhedraDomain>(native);
        report(operation, ApproximationKind::UnsupportedFallback,
               elinaUnsupportedFallbackReason(fallbackOperation), false);
        return;
    }
    fallback_.reset();
    impl_ = std::make_unique<Impl>(native.isBottom());
    ensureVariables(variables);
    if (!native.isBottom() && !constraints.empty())
    {
        bool approximated = false;
        elina_lincons0_array_t array =
            elina_lincons0_array_make(constraints.size());
        for (std::size_t index = 0; index < constraints.size(); ++index)
            array.p[index] = makeConstraint(constraints[index],
                                            impl_->variables, approximated);
        ELINARoundingScope rounding;
        resetResult(impl_->manager);
        elina_abstract0_t* next = elina_abstract0_meet_lincons_array(
            impl_->manager, false, impl_->state, &array);
        elina_lincons0_array_clear(&array);
        const ELINAStatus status = impl_->adopt(next);
        if (!status.ok)
            throw std::runtime_error("ELINA fallback import failed: " +
                                     status.exception);
    }
    report(operation, ApproximationKind::UnsupportedFallback,
           elinaUnsupportedFallbackReason(fallbackOperation), false);
}

void ELINAPolyhedraDomain::assign(Variable target,
                                  const LinearExpression& expression)
{
    std::vector<Variable> variables = expressionVariables(expression);
    variables.push_back(target);
    if (fallback_ || hasNonIntegerVariable(variables))
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.assign(target, expression);
        replaceFromNative(native, OperationKind::Assignment,
                          "non-integer linear assignment");
        return;
    }
    ensureVariables(variables);
    elina_linexpr0_t* raw = makeExpression(expression, impl_->variables);
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_assign_linexpr(
        impl_->manager, false, impl_->state,
        static_cast<elina_dim_t>(dimensionOf(impl_->variables, target)), raw,
        nullptr);
    elina_linexpr0_free(raw);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.assign(target, expression);
        replaceFromNative(native, OperationKind::Assignment,
                          "linear assignment exception");
        return;
    }
    report(OperationKind::Assignment,
           status.exact ? ApproximationKind::Exact
                        : ApproximationKind::SoundOverApproximation,
           status.exact ? std::string() : "ELINA assignment is not exact",
           status.best);
}

void ELINAPolyhedraDomain::assign(Variable target,
                                  const TreeExpression& expression)
{
    if (const auto linear = expression.asLinear())
    {
        assign(target, *linear);
        return;
    }
    ConvexPolyhedraDomain native = nativeSnapshot();
    native.assign(target, expression);
    replaceFromNative(native, OperationKind::Assignment, "tree assignment");
}

void ELINAPolyhedraDomain::assignParallel(
    const LinearAssignmentList& assignments)
{
    if (assignments.empty())
    {
        report(OperationKind::Assignment, ApproximationKind::Exact, {}, true);
        return;
    }
    std::vector<Variable> variables;
    for (const LinearAssignment& assignment : assignments)
    {
        variables.push_back(assignment.target);
        const auto used = expressionVariables(assignment.expression);
        variables.insert(variables.end(), used.begin(), used.end());
    }
    if (fallback_ || hasNonIntegerVariable(variables))
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.assignParallel(assignments);
        replaceFromNative(native, OperationKind::Assignment,
                          "non-integer parallel assignment");
        return;
    }
    ensureVariables(variables);
    std::vector<elina_dim_t> dimensions;
    std::vector<elina_linexpr0_t*> expressions;
    dimensions.reserve(assignments.size());
    expressions.reserve(assignments.size());
    for (const LinearAssignment& assignment : assignments)
    {
        dimensions.push_back(static_cast<elina_dim_t>(
            dimensionOf(impl_->variables, assignment.target)));
        expressions.push_back(
            makeExpression(assignment.expression, impl_->variables));
    }
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_assign_linexpr_array(
        impl_->manager, false, impl_->state, dimensions.data(),
        expressions.data(), assignments.size(), nullptr);
    for (elina_linexpr0_t* expression : expressions)
        elina_linexpr0_free(expression);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.assignParallel(assignments);
        replaceFromNative(native, OperationKind::Assignment,
                          "parallel assignment exception");
        return;
    }
    report(OperationKind::Assignment,
           status.exact ? ApproximationKind::Exact
                        : ApproximationKind::SoundOverApproximation,
           status.exact ? std::string()
                        : "ELINA parallel assignment is not exact",
           status.best);
}

void ELINAPolyhedraDomain::substitute(Variable target,
                                      const LinearExpression& expression)
{
    std::vector<Variable> variables = expressionVariables(expression);
    variables.push_back(target);
    if (fallback_ || hasNonIntegerVariable(variables))
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.substitute(target, expression);
        replaceFromNative(native, OperationKind::Substitution,
                          "non-integer linear substitution");
        return;
    }
    ensureVariables(variables);
    elina_linexpr0_t* raw = makeExpression(expression, impl_->variables);
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_substitute_linexpr(
        impl_->manager, false, impl_->state,
        static_cast<elina_dim_t>(dimensionOf(impl_->variables, target)), raw,
        nullptr);
    elina_linexpr0_free(raw);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.substitute(target, expression);
        replaceFromNative(native, OperationKind::Substitution,
                          "linear substitution exception");
        return;
    }
    report(OperationKind::Substitution,
           status.exact ? ApproximationKind::Exact
                        : ApproximationKind::SoundOverApproximation,
           status.exact ? std::string() : "ELINA substitution is not exact",
           status.best);
}

void ELINAPolyhedraDomain::substituteParallel(
    const LinearAssignmentList& assignments)
{
    if (assignments.empty())
    {
        report(OperationKind::Substitution, ApproximationKind::Exact, {}, true);
        return;
    }
    std::vector<Variable> variables;
    for (const LinearAssignment& assignment : assignments)
    {
        variables.push_back(assignment.target);
        const auto used = expressionVariables(assignment.expression);
        variables.insert(variables.end(), used.begin(), used.end());
    }
    if (fallback_ || hasNonIntegerVariable(variables))
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.substituteParallel(assignments);
        replaceFromNative(native, OperationKind::Substitution,
                          "non-integer parallel substitution");
        return;
    }
    ensureVariables(variables);
    std::vector<elina_dim_t> dimensions;
    std::vector<elina_linexpr0_t*> expressions;
    for (const LinearAssignment& assignment : assignments)
    {
        dimensions.push_back(static_cast<elina_dim_t>(
            dimensionOf(impl_->variables, assignment.target)));
        expressions.push_back(
            makeExpression(assignment.expression, impl_->variables));
    }
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_substitute_linexpr_array(
        impl_->manager, false, impl_->state, dimensions.data(),
        expressions.data(), assignments.size(), nullptr);
    for (elina_linexpr0_t* expression : expressions)
        elina_linexpr0_free(expression);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.substituteParallel(assignments);
        replaceFromNative(native, OperationKind::Substitution,
                          "parallel substitution exception");
        return;
    }
    report(OperationKind::Substitution,
           status.exact ? ApproximationKind::Exact
                        : ApproximationKind::SoundOverApproximation,
           status.exact ? std::string()
                        : "ELINA parallel substitution is not exact",
           status.best);
}

void ELINAPolyhedraDomain::assume(const LinearConstraint& constraint)
{
    const std::vector<Variable> variables =
        expressionVariables(constraint.expression());
    if (fallback_ || hasNonIntegerVariable(variables))
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.assume(constraint);
        replaceFromNative(native, OperationKind::Assumption,
                          "non-integer linear assumption");
        return;
    }
    ensureVariables(variables);
    bool approximated = false;
    elina_lincons0_array_t array = elina_lincons0_array_make(1);
    array.p[0] = makeConstraint(constraint, impl_->variables, approximated);
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_meet_lincons_array(
        impl_->manager, false, impl_->state, &array);
    elina_lincons0_array_clear(&array);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.assume(constraint);
        replaceFromNative(native, OperationKind::Assumption,
                          "linear assumption exception");
        return;
    }
    report(OperationKind::Assumption,
           approximated
               ? ApproximationKind::SoundOverApproximation
               : (status.exact ? ApproximationKind::Exact
                               : ApproximationKind::SoundOverApproximation),
           approximated ? "real strict guard was conservatively closed"
                        : (status.exact ? std::string()
                                        : "ELINA assumption is not exact"),
           !approximated && status.best);
}

void ELINAPolyhedraDomain::assumeAll(const LinearConstraintSet& constraints)
{
    if (constraints.empty())
    {
        report(OperationKind::Assumption, ApproximationKind::Exact, {}, true);
        return;
    }
    std::vector<Variable> variables;
    for (const LinearConstraint& constraint : constraints)
    {
        const auto used = expressionVariables(constraint.expression());
        variables.insert(variables.end(), used.begin(), used.end());
    }
    if (fallback_ || hasNonIntegerVariable(variables))
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.assumeAll(constraints);
        replaceFromNative(native, OperationKind::Assumption,
                          "non-integer batch linear assumption");
        return;
    }
    ensureVariables(variables);
    bool approximated = false;
    elina_lincons0_array_t array =
        elina_lincons0_array_make(constraints.size());
    for (std::size_t index = 0; index < constraints.size(); ++index)
        array.p[index] =
            makeConstraint(constraints[index], impl_->variables, approximated);
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_meet_lincons_array(
        impl_->manager, false, impl_->state, &array);
    elina_lincons0_array_clear(&array);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.assumeAll(constraints);
        replaceFromNative(native, OperationKind::Assumption,
                          "batch linear assumption exception");
        return;
    }
    report(OperationKind::Assumption,
           approximated
               ? ApproximationKind::SoundOverApproximation
               : (status.exact ? ApproximationKind::Exact
                               : ApproximationKind::SoundOverApproximation),
           approximated
               ? "real strict guard was conservatively closed"
               : (status.exact ? std::string()
                               : "ELINA batch assumption is not exact"),
           !approximated && status.best);
}

void ELINAPolyhedraDomain::assume(const TreeConstraint& constraint)
{
    if (const auto linear = constraint.expression().asLinear())
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    ConvexPolyhedraDomain native = nativeSnapshot();
    native.assume(constraint);
    replaceFromNative(native, OperationKind::Assumption, "tree assumption");
}

void ELINAPolyhedraDomain::forget(Variable variable)
{
    if (fallback_)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.forget(variable);
        replaceFromNative(native, OperationKind::Forget,
                          "forget in native fallback state");
        return;
    }
    const auto found =
        std::find(impl_->variables.begin(), impl_->variables.end(), variable);
    if (found == impl_->variables.end())
    {
        report(OperationKind::Forget, ApproximationKind::Exact, {}, true);
        return;
    }
    elina_dim_t dimension =
        static_cast<elina_dim_t>(found - impl_->variables.begin());
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_forget_array(
        impl_->manager, false, impl_->state, &dimension, 1, false);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.forget(variable);
        replaceFromNative(native, OperationKind::Forget, "forget exception");
        return;
    }
    removeVariables({variable});
    report(OperationKind::Forget,
           status.exact ? ApproximationKind::Exact
                        : ApproximationKind::SoundOverApproximation,
           status.exact ? std::string() : "ELINA forget is not exact",
           status.best);
}

void ELINAPolyhedraDomain::project(const std::vector<Variable>& retained)
{
    if (fallback_)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.project(retained);
        replaceFromNative(native, OperationKind::Forget,
                          "project in native fallback state");
        return;
    }
    std::vector<Variable> removed;
    for (Variable variable : impl_->variables)
    {
        if (std::find(retained.begin(), retained.end(), variable) ==
            retained.end())
            removed.push_back(variable);
    }
    removeVariables(removed);
    report(OperationKind::Forget, ApproximationKind::Exact, {}, true);
}

void ELINAPolyhedraDomain::expand(Variable source,
                                  const std::vector<Variable>& copies)
{
    ConvexPolyhedraDomain native = nativeSnapshot();
    native.expand(source, copies);
    replaceFromNative(native, OperationKind::Expand, "expand");
}

void ELINAPolyhedraDomain::fold(Variable target,
                                const std::vector<Variable>& folded)
{
    ConvexPolyhedraDomain native = nativeSnapshot();
    native.fold(target, folded);
    replaceFromNative(native, OperationKind::Fold, "fold");
}

CheckResult ELINAPolyhedraDomain::entails(
    const LinearConstraint& constraint) const
{
    const std::vector<Variable> variables =
        expressionVariables(constraint.expression());
    if (fallback_ || hasNonIntegerVariable(variables))
    {
        report(OperationKind::Assumption,
               ApproximationKind::UnsupportedFallback,
               elinaUnsupportedFallbackReason("non-integer entailment query"),
               false);
        return nativeSnapshot().entails(constraint);
    }
    ELINAPolyhedraDomain query(*this);
    query.ensureVariables(variables);
    bool approximated = false;
    elina_lincons0_t raw =
        makeConstraint(constraint, query.impl_->variables, approximated);
    if (approximated)
    {
        elina_lincons0_clear(&raw);
        return CheckResult::Unknown;
    }
    ELINARoundingScope rounding;
    resetResult(query.impl_->manager);
    const bool value = elina_abstract0_sat_lincons(query.impl_->manager,
                                                   query.impl_->state, &raw);
    elina_lincons0_clear(&raw);
    return classifyELINAPredicate(
        value, query.impl_->manager->result.flag_exact,
        query.impl_->manager->result.flag_best,
        query.impl_->manager->result.exn != ELINA_EXC_NONE);
}

Interval ELINAPolyhedraDomain::bound(Variable variable) const
{
    if (fallback_)
        return fallback_->bound(variable);
    if (variable.type().kind != NumericKind::Integer)
        return Interval::top();
    const auto found =
        std::find(impl_->variables.begin(), impl_->variables.end(), variable);
    if (found == impl_->variables.end())
        return Interval::top();
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_interval_t* raw = elina_abstract0_bound_dimension(
        impl_->manager, impl_->state,
        static_cast<elina_dim_t>(found - impl_->variables.begin()));
    if (!raw || impl_->manager->result.exn != ELINA_EXC_NONE)
    {
        if (raw)
            elina_interval_free(raw);
        report(
            OperationKind::Assumption, ApproximationKind::UnsupportedFallback,
            elinaUnsupportedFallbackReason("dimension bound exception"), false);
        return nativeSnapshot().bound(variable);
    }
    const Interval result = intervalFromELINA(raw);
    elina_interval_free(raw);
    return result;
}

Interval ELINAPolyhedraDomain::bound(const LinearExpression& expression) const
{
    const std::vector<Variable> variables = expressionVariables(expression);
    if (fallback_ || hasNonIntegerVariable(variables))
        return nativeSnapshot().bound(expression);
    ELINAPolyhedraDomain query(*this);
    query.ensureVariables(variables);
    elina_linexpr0_t* raw = makeExpression(expression, query.impl_->variables);
    ELINARoundingScope rounding;
    resetResult(query.impl_->manager);
    elina_interval_t* interval = elina_abstract0_bound_linexpr(
        query.impl_->manager, query.impl_->state, raw);
    elina_linexpr0_free(raw);
    if (!interval || query.impl_->manager->result.exn != ELINA_EXC_NONE)
    {
        if (interval)
            elina_interval_free(interval);
        report(OperationKind::Assumption,
               ApproximationKind::UnsupportedFallback,
               elinaUnsupportedFallbackReason("linear bound exception"), false);
        return nativeSnapshot().bound(expression);
    }
    const Interval result = intervalFromELINA(interval);
    elina_interval_free(interval);
    return result;
}

std::vector<Variable> ELINAPolyhedraDomain::supportVariables() const
{
    if (fallback_)
        return fallback_->supportVariables();
    std::vector<Variable> result;
    ELINARoundingScope rounding;
    for (std::size_t dimension = 0; dimension < impl_->variables.size();
         ++dimension)
    {
        resetResult(impl_->manager);
        const bool unconstrained = elina_abstract0_is_dimension_unconstrained(
            impl_->manager, impl_->state, static_cast<elina_dim_t>(dimension));
        if (!unconstrained || impl_->manager->result.exn != ELINA_EXC_NONE)
            result.push_back(impl_->variables[dimension]);
    }
    std::sort(result.begin(), result.end());
    return result;
}

LinearConstraintSet ELINAPolyhedraDomain::toConstraints() const
{
    if (fallback_)
        return fallback_->toConstraints();
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_lincons0_array_t raw =
        elina_abstract0_to_lincons_array(impl_->manager, impl_->state);
    if (impl_->manager->result.exn != ELINA_EXC_NONE)
    {
        elina_lincons0_array_clear(&raw);
        throw std::runtime_error("ELINA constraint export failed");
    }
    LinearConstraintSet result;
    result.reserve(raw.size);
    for (std::size_t index = 0; index < raw.size; ++index)
        result.push_back(constraintFromELINA(raw.p[index], impl_->variables));
    elina_lincons0_array_clear(&raw);
    return result;
}

void ELINAPolyhedraDomain::close()
{
    ConvexPolyhedraDomain native = nativeSnapshot();
    native.close();
    replaceFromNative(native, OperationKind::TopologicalClosure,
                      "topological closure");
}

void ELINAPolyhedraDomain::canonicalize()
{
    if (fallback_)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.canonicalize();
        replaceFromNative(native, OperationKind::Canonicalization,
                          "canonicalization in native fallback state");
        return;
    }
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_canonicalize(impl_->manager, impl_->state);
    if (impl_->manager->result.exn == ELINA_EXC_NONE)
    {
        report(OperationKind::Canonicalization, ApproximationKind::Exact, {},
               true);
        return;
    }
    ConvexPolyhedraDomain native = nativeSnapshot();
    native.canonicalize();
    replaceFromNative(native, OperationKind::Canonicalization,
                      "canonicalization exception");
}

CheckResult ELINAPolyhedraDomain::backendInclusion(
    const ELINAPolyhedraDomain& other) const
{
    if (fallback_ || other.fallback_)
        return nativeSnapshot().isSubsetOf(other.nativeSnapshot());
    ELINAPolyhedraDomain lhs(*this);
    ELINAPolyhedraDomain rhs(other);
    std::vector<Variable> variables = lhs.impl_->variables;
    variables.insert(variables.end(), rhs.impl_->variables.begin(),
                     rhs.impl_->variables.end());
    variables = orderedUnique(std::move(variables));
    lhs.ensureVariables(variables);
    rhs.ensureVariables(variables);
    ELINARoundingScope rounding;
    resetResult(lhs.impl_->manager);
    const bool result = elina_abstract0_is_leq(
        lhs.impl_->manager, lhs.impl_->state, rhs.impl_->state);
    return classifyELINAPredicate(result, lhs.impl_->manager->result.flag_exact,
                                  lhs.impl_->manager->result.flag_best,
                                  lhs.impl_->manager->result.exn !=
                                      ELINA_EXC_NONE);
}

const ELINAPolyhedraDomain& ELINAPolyhedraDomain::requireELINA(
    const AbstractDomain& other) const
{
    requireCompatible(other);
    return static_cast<const ELINAPolyhedraDomain&>(other);
}

bool ELINAPolyhedraDomain::hasCompatibleDomain(
    const AbstractDomain& other) const
{
    return other.isDomain<ELINAPolyhedraDomain>() &&
           config_.operationCompatible(
               static_cast<const ELINAPolyhedraDomain&>(other).config_);
}

void ELINAPolyhedraDomain::joinDomain(const AbstractDomain& other)
{
    ELINAPolyhedraDomain rhs(requireELINA(other));
    if (fallback_ || rhs.fallback_)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.joinWith(rhs.nativeSnapshot());
        replaceFromNative(native, OperationKind::Join,
                          "join with native fallback state");
        return;
    }
    std::vector<Variable> variables = impl_->variables;
    variables.insert(variables.end(), rhs.impl_->variables.begin(),
                     rhs.impl_->variables.end());
    variables = orderedUnique(std::move(variables));
    ensureVariables(variables);
    rhs.ensureVariables(variables);
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_join(
        impl_->manager, false, impl_->state, rhs.impl_->state);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.joinWith(rhs.nativeSnapshot());
        replaceFromNative(native, OperationKind::Join, "join exception");
        return;
    }
    report(OperationKind::Join,
           status.exact ? ApproximationKind::Exact
                        : ApproximationKind::SoundOverApproximation,
           status.exact ? std::string() : "ELINA join is not exact",
           status.best);
}

void ELINAPolyhedraDomain::meetDomain(const AbstractDomain& other)
{
    ELINAPolyhedraDomain rhs(requireELINA(other));
    if (fallback_ || rhs.fallback_)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.meetWith(rhs.nativeSnapshot());
        replaceFromNative(native, OperationKind::Meet,
                          "meet with native fallback state");
        return;
    }
    std::vector<Variable> variables = impl_->variables;
    variables.insert(variables.end(), rhs.impl_->variables.begin(),
                     rhs.impl_->variables.end());
    variables = orderedUnique(std::move(variables));
    ensureVariables(variables);
    rhs.ensureVariables(variables);
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* next = elina_abstract0_meet(
        impl_->manager, false, impl_->state, rhs.impl_->state);
    const ELINAStatus status = impl_->adopt(next);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.meetWith(rhs.nativeSnapshot());
        replaceFromNative(native, OperationKind::Meet, "meet exception");
        return;
    }
    report(OperationKind::Meet,
           status.exact ? ApproximationKind::Exact
                        : ApproximationKind::SoundOverApproximation,
           status.exact ? std::string() : "ELINA meet is not exact",
           status.best);
}

void ELINAPolyhedraDomain::widenDomain(const AbstractDomain& nextDomain)
{
    ELINAPolyhedraDomain next(requireELINA(nextDomain));
    if (fallback_ || next.fallback_)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.widenWith(next.nativeSnapshot());
        replaceFromNative(native, OperationKind::Widening,
                          "widening with native fallback state");
        return;
    }
    std::vector<Variable> variables = impl_->variables;
    variables.insert(variables.end(), next.impl_->variables.begin(),
                     next.impl_->variables.end());
    variables = orderedUnique(std::move(variables));
    ensureVariables(variables);
    next.ensureVariables(variables);
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    elina_abstract0_t* widened = elina_abstract0_widening(
        impl_->manager, impl_->state, next.impl_->state);
    const ELINAStatus status = impl_->adopt(widened);
    if (!status.ok)
    {
        ConvexPolyhedraDomain native = nativeSnapshot();
        native.widenWith(next.nativeSnapshot());
        replaceFromNative(native, OperationKind::Widening,
                          "widening exception");
        return;
    }
    report(OperationKind::Widening,
           status.exact ? ApproximationKind::Exact
                        : ApproximationKind::SoundOverApproximation,
           status.exact ? std::string() : "ELINA widening is not exact",
           status.best);
}

void ELINAPolyhedraDomain::narrowDomain(const AbstractDomain& nextDomain)
{
    ELINAPolyhedraDomain next(requireELINA(nextDomain));
    meetDomain(next);
    report(OperationKind::Narrowing, ApproximationKind::SoundOverApproximation,
           "ELINA meet used as narrowing", false);
}

bool ELINAPolyhedraDomain::isBottomDomain() const
{
    if (fallback_)
        return fallback_->isBottom();
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    return elina_abstract0_is_bottom(impl_->manager, impl_->state) &&
           impl_->manager->result.exn == ELINA_EXC_NONE;
}

bool ELINAPolyhedraDomain::isTopDomain() const
{
    if (fallback_)
        return fallback_->isTop();
    ELINARoundingScope rounding;
    resetResult(impl_->manager);
    return elina_abstract0_is_top(impl_->manager, impl_->state) &&
           impl_->manager->result.exn == ELINA_EXC_NONE;
}

bool ELINAPolyhedraDomain::leqDomain(const AbstractDomain& other) const
{
    return backendInclusion(requireELINA(other)) == CheckResult::True;
}

std::string ELINAPolyhedraDomain::domainToString() const
{
    if (fallback_)
        return "elina-polyhedra-fallback:" + fallback_->toString();
    std::ostringstream output;
    output << "elina-polyhedra:{";
    const LinearConstraintSet constraints = toConstraints();
    for (std::size_t index = 0; index < constraints.size(); ++index)
    {
        if (index != 0)
            output << ", ";
        output << constraints[index].toString();
    }
    output << '}';
    return output.str();
}

} // namespace SVF::AbstractDomain
