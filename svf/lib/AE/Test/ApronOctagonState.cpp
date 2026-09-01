//===- ApronOctagonState.cpp -- Benchmark-only APRON adapter -------------===//

#include "ApronOctagonState.h"

extern "C"
{
#include "oct.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SVF::AbstractDomain
{
namespace
{

class OctagonManager
{
public:
    OctagonManager() : manager_(oct_manager_alloc())
    {
        if (!manager_)
            throw std::runtime_error("cannot allocate APRON octMPQ manager");
    }
    ~OctagonManager() { ap_manager_free(manager_); }
    ap_manager_t* get() const { return manager_; }

private:
    ap_manager_t* manager_;
};

ap_manager_t* manager()
{
    static OctagonManager instance;
    return instance.get();
}

std::string variableName(Variable variable)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "svf_v_%010u", variable.id());
    return buffer;
}

ap_var_t apronVariable(Variable variable, std::string& storage)
{
    storage = variableName(variable);
    return const_cast<char*>(storage.c_str());
}

ap_environment_t* makeEnvironment(const VariableEnvironment& environment)
{
    std::vector<std::string> integerNames;
    std::vector<std::string> realNames;
    for (const VariableDeclaration& declaration : environment.variables())
    {
        auto& names = declaration.type.kind == NumericKind::Integer
                          ? integerNames
                          : realNames;
        names.push_back(variableName(declaration.variable));
    }
    std::sort(integerNames.begin(), integerNames.end());
    std::sort(realNames.begin(), realNames.end());
    std::vector<ap_var_t> integers;
    std::vector<ap_var_t> reals;
    for (std::string& name : integerNames)
        integers.push_back(const_cast<char*>(name.c_str()));
    for (std::string& name : realNames)
        reals.push_back(const_cast<char*>(name.c_str()));
    ap_environment_t* result = ap_environment_alloc(
        integers.data(), integers.size(), reals.data(), reals.size());
    if (!result)
        throw std::runtime_error("cannot allocate APRON environment");
    return result;
}

ap_dim_t dimensionOf(ap_environment_t* environment, Variable variable)
{
    std::string name;
    const ap_dim_t dimension =
        ap_environment_dim_of_var(environment, apronVariable(variable, name));
    if (dimension == AP_DIM_MAX)
        throw std::invalid_argument("variable is not in APRON environment");
    return dimension;
}

ap_scalar_t* scalar(const Rational& value)
{
    return ap_scalar_alloc_set_mpq(
        const_cast<mpq_ptr>(value.value().get_mpq_t()));
}

ap_linexpr0_t* expression(const LinearExpression& source,
                          ap_environment_t* environment, bool negate = false)
{
    const std::size_t size = environment->intdim + environment->realdim;
    ap_linexpr0_t* result = ap_linexpr0_alloc(AP_LINEXPR_DENSE, size);
    const Rational sign(negate ? -1 : 1);
    for (const auto& [variable, coefficient] : source.terms())
    {
        ap_scalar_t* value = scalar(coefficient * sign);
        ap_linexpr0_set_coeff_scalar(result, dimensionOf(environment, variable),
                                     value);
        ap_scalar_free(value);
    }
    ap_scalar_t* constant = scalar(source.constant() * sign);
    ap_linexpr0_set_cst_scalar(result, constant);
    ap_scalar_free(constant);
    return result;
}

ap_lincons0_t constraint(const LinearConstraint& source,
                         ap_environment_t* environment)
{
    ap_constyp_t type = AP_CONS_EQ;
    bool negate = false;
    switch (source.kind())
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
    return ap_lincons0_make(type, expression(source.expression(), environment,
                                             negate),
                            nullptr);
}

ap_lincons0_array_t constraints(const LinearConstraintSet& source,
                                ap_environment_t* environment)
{
    ap_lincons0_array_t result = ap_lincons0_array_make(source.size());
    for (std::size_t index = 0; index < source.size(); ++index)
        result.p[index] = constraint(source[index], environment);
    return result;
}

Rational rational(ap_scalar_t* value)
{
    mpq_class converted;
    ap_mpq_set_scalar(converted.get_mpq_t(), value, GMP_RNDN);
    return Rational::fromRaw(converted);
}

Rational coefficient(ap_coeff_t* value)
{
    if (value->discr != AP_COEFF_SCALAR)
        throw std::runtime_error(
            "APRON Octagon exported an interval coefficient");
    return rational(value->val.scalar);
}

Variable variableOf(ap_environment_t* environment, ap_dim_t dimension)
{
    const char* name = static_cast<const char*>(
        ap_environment_var_of_dim(environment, dimension));
    constexpr const char* prefix = "svf_v_";
    if (!name || std::string(name).compare(0, 6, prefix) != 0)
        throw std::runtime_error("unexpected APRON variable name");
    char* end = nullptr;
    const unsigned long id = std::strtoul(name + 6, &end, 10);
    if (!end || *end != '\0' || id > std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("invalid APRON variable name");
    return Variable(static_cast<std::uint32_t>(id));
}

Interval interval(ap_interval_t* value)
{
    if (ap_interval_is_bottom(value))
        return Interval(Bound::plusInfinity(), Bound::minusInfinity());
    const int lowerInfinity = ap_scalar_infty(value->inf);
    const int upperInfinity = ap_scalar_infty(value->sup);
    Bound lower = lowerInfinity < 0 ? Bound::minusInfinity()
                  : lowerInfinity > 0 ? Bound::plusInfinity()
                                      : Bound::finite(rational(value->inf));
    Bound upper = upperInfinity > 0 ? Bound::plusInfinity()
                  : upperInfinity < 0 ? Bound::minusInfinity()
                                      : Bound::finite(rational(value->sup));
    return Interval(std::move(lower), std::move(upper));
}

const OctagonConfig& adapterConfig()
{
    static const OctagonConfig config{};
    return config;
}

} // namespace

ApronOctagonState ApronOctagonState::top(
    const VariableEnvironment& environment)
{
    ap_environment_t* apronEnvironment = makeEnvironment(environment);
    ap_abstract1_t value = ap_abstract1_top(manager(), apronEnvironment);
    ap_environment_free(apronEnvironment);
    return ApronOctagonState(environment, value);
}

ApronOctagonState ApronOctagonState::bottom(
    const VariableEnvironment& environment)
{
    ap_environment_t* apronEnvironment = makeEnvironment(environment);
    ap_abstract1_t value = ap_abstract1_bottom(manager(), apronEnvironment);
    ap_environment_free(apronEnvironment);
    return ApronOctagonState(environment, value);
}

ApronOctagonState::ApronOctagonState(VariableEnvironment environment,
                                     ap_abstract1_t value)
    : environment_(std::move(environment)), value_(value)
{
}

ApronOctagonState::ApronOctagonState(const ApronOctagonState& other)
    : environment_(other.environment_),
      value_(ap_abstract1_copy(manager(),
                              const_cast<ap_abstract1_t*>(&other.value_)))
{
}

ApronOctagonState::ApronOctagonState(ApronOctagonState&& other) noexcept
    : environment_(std::move(other.environment_)), value_(other.value_)
{
    other.value_ = {nullptr, nullptr};
}

ApronOctagonState& ApronOctagonState::operator=(
    ApronOctagonState other) noexcept
{
    swap(other);
    return *this;
}

ApronOctagonState::~ApronOctagonState()
{
    if (value_.abstract0)
        ap_abstract1_clear(manager(), &value_);
}

void ApronOctagonState::swap(ApronOctagonState& other) noexcept
{
    std::swap(environment_, other.environment_);
    std::swap(value_, other.value_);
}

std::unique_ptr<AbstractState> ApronOctagonState::clone() const
{
    return std::make_unique<ApronOctagonState>(*this);
}

const char* ApronOctagonState::name() const { return "APRON Octagon octMPQ"; }

DomainCapabilities ApronOctagonState::capabilities() const
{
    DomainCapabilities result;
    result.strictInequalities = true;
    result.integerTightening = true;
    result.parallelAssignments = true;
    result.expressionBounds = true;
    result.backwardAssignments = true;
    result.topologicalClosure = true;
    result.canonicalization = true;
    result.expandFold = true;
    result.narrowing = true;
    return result;
}

const VariableEnvironment& ApronOctagonState::environment() const
{
    return environment_;
}

const OctagonConfig& ApronOctagonState::config() const
{
    return adapterConfig();
}

std::uint64_t ApronOctagonState::hash() const
{
    return OctagonState::fromConstraints(environment_, toConstraints()).hash();
}

void ApronOctagonState::replace(ap_abstract1_t value)
{
    if (value_.abstract0)
        ap_abstract1_clear(manager(), &value_);
    value_ = value;
}

void ApronOctagonState::assign(Variable target,
                               const LinearExpression& source)
{
    ap_linexpr0_t* apronExpression = expression(source, value_.env);
    ap_abstract0_t* result = ap_abstract0_assign_linexpr(
        manager(), false, value_.abstract0, dimensionOf(value_.env, target),
        apronExpression, nullptr);
    ap_linexpr0_free(apronExpression);
    replace({result, ap_environment_copy(value_.env)});
}

void ApronOctagonState::assign(Variable target,
                               const TreeExpression& source)
{
    if (const auto linear = source.asLinear())
        assign(target, *linear);
    else
        assignInterval(target, evaluateTreeExpression(source));
}

void ApronOctagonState::assignParallel(
    const LinearAssignmentList& assignments)
{
    if (assignments.empty())
        return;
    std::vector<ap_dim_t> dimensions;
    std::vector<ap_linexpr0_t*> expressions;
    for (const LinearAssignment& assignment : assignments)
    {
        dimensions.push_back(dimensionOf(value_.env, assignment.target));
        expressions.push_back(expression(assignment.expression, value_.env));
    }
    ap_abstract0_t* result = ap_abstract0_assign_linexpr_array(
        manager(), false, value_.abstract0, dimensions.data(),
        expressions.data(), expressions.size(), nullptr);
    for (ap_linexpr0_t* item : expressions)
        ap_linexpr0_free(item);
    replace({result, ap_environment_copy(value_.env)});
}

void ApronOctagonState::substitute(Variable target,
                                   const LinearExpression& source)
{
    substituteParallel({{target, source}});
}

void ApronOctagonState::substituteParallel(
    const LinearAssignmentList& assignments)
{
    if (assignments.empty())
        return;
    std::vector<ap_dim_t> dimensions;
    std::vector<ap_linexpr0_t*> expressions;
    for (const LinearAssignment& assignment : assignments)
    {
        dimensions.push_back(dimensionOf(value_.env, assignment.target));
        expressions.push_back(expression(assignment.expression, value_.env));
    }
    ap_abstract0_t* result = ap_abstract0_substitute_linexpr_array(
        manager(), false, value_.abstract0, dimensions.data(),
        expressions.data(), expressions.size(), nullptr);
    for (ap_linexpr0_t* item : expressions)
        ap_linexpr0_free(item);
    replace({result, ap_environment_copy(value_.env)});
}

void ApronOctagonState::assume(const LinearConstraint& source)
{
    assumeAll({source});
}

void ApronOctagonState::assume(const TreeConstraint& source)
{
    if (const auto linear = source.expression().asLinear())
        assume(LinearConstraint(*linear, source.kind()));
    else
        assumeAll(treeConstraintConsequences(source));
}

void ApronOctagonState::assumeAll(const LinearConstraintSet& source)
{
    if (source.empty())
        return;
    ap_lincons0_array_t array = constraints(source, value_.env);
    ap_abstract0_t* result = ap_abstract0_meet_lincons_array(
        manager(), false, value_.abstract0, &array);
    ap_lincons0_array_clear(&array);
    replace({result, ap_environment_copy(value_.env)});
}

void ApronOctagonState::forget(Variable variable)
{
    ap_dim_t dimension = dimensionOf(value_.env, variable);
    ap_abstract0_t* result = ap_abstract0_forget_array(
        manager(), false, value_.abstract0, &dimension, 1, false);
    replace({result, ap_environment_copy(value_.env)});
}

void ApronOctagonState::changeEnvironment(
    const VariableEnvironment& environment, bool initializeNewVariablesToZero)
{
    if (environment_ == environment)
        return;
    ap_environment_t* apronEnvironment = makeEnvironment(environment);
    ap_abstract1_t result = ap_abstract1_change_environment(
        manager(), false, &value_, apronEnvironment,
        initializeNewVariablesToZero);
    ap_environment_free(apronEnvironment);
    replace(result);
    environment_ = environment;
}

void ApronOctagonState::expand(
    Variable source, const std::vector<VariableDeclaration>& copies)
{
    if (copies.empty())
        return;
    std::string sourceName;
    std::vector<std::string> names;
    std::vector<ap_var_t> variables;
    for (const VariableDeclaration& copy : copies)
        names.push_back(variableName(copy.variable));
    for (std::string& name : names)
        variables.push_back(const_cast<char*>(name.c_str()));
    ap_abstract1_t result = ap_abstract1_expand(
        manager(), false, &value_, apronVariable(source, sourceName),
        variables.data(), variables.size());
    replace(result);
    environment_ = environment_.add(copies);
}

void ApronOctagonState::fold(Variable target,
                             const std::vector<Variable>& folded)
{
    if (folded.empty())
        return;
    std::vector<std::string> names;
    std::vector<ap_var_t> variables;
    names.push_back(variableName(target));
    for (Variable variable : folded)
        if (variable != target)
            names.push_back(variableName(variable));
    for (std::string& name : names)
        variables.push_back(const_cast<char*>(name.c_str()));
    ap_abstract1_t result = ap_abstract1_fold(
        manager(), false, &value_, variables.data(), variables.size());
    replace(result);
    std::vector<Variable> removed;
    for (Variable variable : folded)
        if (variable != target)
            removed.push_back(variable);
    environment_ = environment_.remove(removed);
}

CheckResult ApronOctagonState::entails(
    const LinearConstraint& source) const
{
    ap_lincons0_t apronConstraint = constraint(source, value_.env);
    const bool result = ap_abstract0_sat_lincons(
        manager(), value_.abstract0, &apronConstraint);
    ap_lincons0_clear(&apronConstraint);
    return result ? CheckResult::True : CheckResult::Unknown;
}

Interval ApronOctagonState::bound(Variable variable) const
{
    ap_interval_t* result = ap_abstract0_bound_dimension(
        manager(), value_.abstract0, dimensionOf(value_.env, variable));
    Interval converted = interval(result);
    ap_interval_free(result);
    return converted;
}

Interval ApronOctagonState::bound(const LinearExpression& source) const
{
    ap_linexpr0_t* apronExpression = expression(source, value_.env);
    ap_interval_t* result = ap_abstract0_bound_linexpr(
        manager(), value_.abstract0, apronExpression);
    ap_linexpr0_free(apronExpression);
    Interval converted = interval(result);
    ap_interval_free(result);
    return converted;
}

IntervalBox ApronOctagonState::toBox() const
{
    IntervalBox result;
    for (const VariableDeclaration& declaration : environment_.variables())
        result.bounds.emplace(declaration.variable, bound(declaration.variable));
    return result;
}

LinearConstraintSet ApronOctagonState::toConstraints() const
{
    LinearConstraintSet result;
    ap_lincons0_array_t array =
        ap_abstract0_to_lincons_array(manager(), value_.abstract0);
    result.reserve(array.size);
    for (std::size_t constraintIndex = 0; constraintIndex < array.size;
         ++constraintIndex)
    {
        ap_lincons0_t& source = array.p[constraintIndex];
        LinearExpression converted;
        std::size_t termIndex = 0;
        ap_dim_t dimension = 0;
        ap_coeff_t* term = nullptr;
        ap_linexpr0_ForeachLinterm(source.linexpr0, termIndex, dimension,
                                  term)
        {
            if (!ap_coeff_zero(term))
                converted.setCoefficient(variableOf(value_.env, dimension),
                                         coefficient(term));
        }
        converted.setConstant(coefficient(&source.linexpr0->cst));
        ConstraintKind kind = ConstraintKind::Equal;
        switch (source.constyp)
        {
        case AP_CONS_EQ:
            kind = ConstraintKind::Equal;
            break;
        case AP_CONS_DISEQ:
            kind = ConstraintKind::NotEqual;
            break;
        case AP_CONS_SUPEQ:
            kind = ConstraintKind::GreaterEqual;
            break;
        case AP_CONS_SUP:
            kind = ConstraintKind::GreaterThan;
            break;
        case AP_CONS_EQMOD:
            ap_lincons0_array_clear(&array);
            throw std::runtime_error(
                "APRON Octagon exported a modular equality");
        }
        result.emplace_back(std::move(converted), kind);
    }
    ap_lincons0_array_clear(&array);
    return result;
}

void ApronOctagonState::close()
{
    ap_abstract1_t result = ap_abstract1_closure(manager(), false, &value_);
    replace(result);
}

void ApronOctagonState::canonicalize()
{
    ap_abstract1_canonicalize(manager(), &value_);
}

void ApronOctagonState::assignInterval(Variable target,
                                       const Interval& assigned)
{
    if (assigned.isBottom())
    {
        *this = bottom(environment_);
        return;
    }
    forget(target);
    LinearConstraintSet refinements;
    if (assigned.lower().isFinite())
    {
        LinearExpression lower(target);
        lower.setConstant(-assigned.lower().value());
        refinements.emplace_back(
            std::move(lower), assigned.lower().isStrict()
                                  ? ConstraintKind::GreaterThan
                                  : ConstraintKind::GreaterEqual);
    }
    if (assigned.upper().isFinite())
    {
        LinearExpression upper(target);
        upper.setConstant(-assigned.upper().value());
        refinements.emplace_back(
            std::move(upper), assigned.upper().isStrict()
                                  ? ConstraintKind::LessThan
                                  : ConstraintKind::LessEqual);
    }
    assumeAll(refinements);
}

bool ApronOctagonState::hasCompatibleDomain(
    const AbstractState& other) const
{
    const auto* apron =
        other.isState<ApronOctagonState>()
            ? &static_cast<const ApronOctagonState&>(other)
            : nullptr;
    return apron && environment_ == apron->environment_;
}

const ApronOctagonState& ApronOctagonState::requireApron(
    const AbstractState& other) const
{
    requireCompatible(other);
    return static_cast<const ApronOctagonState&>(other);
}

void ApronOctagonState::joinState(const AbstractState& other)
{
    const ApronOctagonState& rhs = requireApron(other);
    replace(ap_abstract1_join(manager(), false, &value_,
                              const_cast<ap_abstract1_t*>(&rhs.value_)));
}

void ApronOctagonState::meetState(const AbstractState& other)
{
    const ApronOctagonState& rhs = requireApron(other);
    replace(ap_abstract1_meet(manager(), false, &value_,
                              const_cast<ap_abstract1_t*>(&rhs.value_)));
}

void ApronOctagonState::widenState(const AbstractState& next)
{
    const ApronOctagonState& rhs = requireApron(next);
    replace(ap_abstract1_widening(
        manager(), &value_, const_cast<ap_abstract1_t*>(&rhs.value_)));
}

void ApronOctagonState::narrowState(const AbstractState& next)
{
    const ApronOctagonState& rhs = requireApron(next);
    ap_abstract0_t* result = ap_abstract0_oct_narrowing(
        manager(), value_.abstract0, rhs.value_.abstract0);
    replace({result, ap_environment_copy(value_.env)});
}

bool ApronOctagonState::isBottomState() const
{
    return ap_abstract1_is_bottom(manager(),
                                  const_cast<ap_abstract1_t*>(&value_));
}

bool ApronOctagonState::isTopState() const
{
    return ap_abstract1_is_top(manager(),
                               const_cast<ap_abstract1_t*>(&value_));
}

bool ApronOctagonState::leqState(const AbstractState& other) const
{
    const ApronOctagonState& rhs = requireApron(other);
    return ap_abstract1_is_leq(
        manager(), const_cast<ap_abstract1_t*>(&value_),
        const_cast<ap_abstract1_t*>(&rhs.value_));
}

std::string ApronOctagonState::stateToString() const
{
    if (isBottomState())
        return "bottom";
    if (isTopState())
        return "top";
    std::ostringstream output;
    output << "APRON octMPQ {";
    bool first = true;
    for (const auto& [variable, value] : toBox().bounds)
    {
        if (!first)
            output << ", ";
        first = false;
        output << variableName(variable) << '=' << value.toString();
    }
    output << '}';
    return output.str();
}

} // namespace SVF::AbstractDomain
