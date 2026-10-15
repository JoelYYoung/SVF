//===- ElinaOctagonDomain.cpp -- ELINA Octagon adapter -----------------===//

#include "AE/Core/ElinaOctagonDomain.h"

#include "elina_abstract0.h"
#include "opt_oct.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace SVF::AbstractDomain
{

namespace
{

class ScopedElinaRounding
{
public:
    ScopedElinaRounding() : previous_(std::fegetround())
    {
        if (previous_ == -1 || std::fesetround(FE_UPWARD) != 0 ||
                std::fegetround() != FE_UPWARD)
            throw std::runtime_error(
                "ELINA Octagon could not establish FE_UPWARD");
    }

    ~ScopedElinaRounding()
    {
        if (previous_ != -1)
            (void)std::fesetround(previous_);
    }

    ScopedElinaRounding(const ScopedElinaRounding&) = delete;
    ScopedElinaRounding& operator=(const ScopedElinaRounding&) = delete;

private:
    int previous_;
};

bool fpuRoundTrip() noexcept
{
    const int previous = std::fegetround();
    if (previous == -1)
        return false;
    const bool supported = std::fesetround(FE_UPWARD) == 0 &&
                           std::fegetround() == FE_UPWARD;
    const bool restored = std::fesetround(previous) == 0;
    return supported && restored;
}

bool integerDimension(Variable variable)
{
    return variable.type().kind == NumericKind::Integer;
}

void resetResult(elina_manager_t* manager)
{
    elina_manager_clear_exclog(manager);
    manager->result.exn = ELINA_EXC_NONE;
    manager->result.flag_exact = false;
    manager->result.flag_best = false;
}

bool succeeded(elina_manager_t* manager)
{
    return manager->result.exn == ELINA_EXC_NONE;
}

std::string exceptionReason(elina_manager_t* manager,
                            const char* operation)
{
    std::ostringstream output;
    output << "ELINA Octagon " << operation << " raised ";
    const auto exception = manager->result.exn;
    if (exception >= ELINA_EXC_NONE && exception < ELINA_EXC_SIZE)
        output << elina_name_of_exception[exception];
    else
        output << "an unknown exception";
    return output.str();
}

Rational scalarToRational(const elina_scalar_t* scalar)
{
    if (elina_scalar_infty(const_cast<elina_scalar_t*>(scalar)) != 0)
        throw std::invalid_argument("infinite ELINA scalar is not rational");
    mpq_class value;
    switch (scalar->discr)
    {
    case ELINA_SCALAR_DOUBLE:
        return Rational::fromDouble(scalar->val.dbl);
    case ELINA_SCALAR_MPQ:
        mpq_set(value.get_mpq_t(), scalar->val.mpq);
        return Rational::fromRaw(value);
    case ELINA_SCALAR_MPFR:
        mpfr_get_q(value.get_mpq_t(), scalar->val.mpfr);
        return Rational::fromRaw(value);
    }
    throw std::invalid_argument("unknown ELINA scalar representation");
}

elina_scalar_t* rationalToScalar(const Rational& value)
{
    elina_scalar_t* scalar = elina_scalar_alloc();
    mpq_class copy = value.value();
    elina_scalar_set_mpq(scalar, copy.get_mpq_t());
    return scalar;
}

Bound lowerBound(const elina_scalar_t* scalar)
{
    const int infinity =
        elina_scalar_infty(const_cast<elina_scalar_t*>(scalar));
    if (infinity < 0)
        return Bound::minusInfinity();
    if (infinity > 0)
        return Bound::plusInfinity();
    return Bound::finite(scalarToRational(scalar));
}

Bound upperBound(const elina_scalar_t* scalar)
{
    const int infinity =
        elina_scalar_infty(const_cast<elina_scalar_t*>(scalar));
    if (infinity < 0)
        return Bound::minusInfinity();
    if (infinity > 0)
        return Bound::plusInfinity();
    return Bound::finite(scalarToRational(scalar));
}

std::optional<Rational> coefficientValue(elina_coeff_t* coefficient)
{
    if (coefficient->discr == ELINA_COEFF_SCALAR)
        return scalarToRational(coefficient->val.scalar);
    elina_interval_t* interval = coefficient->val.interval;
    if (!elina_scalar_equal(interval->inf, interval->sup))
        return std::nullopt;
    return scalarToRational(interval->inf);
}

bool integralExpression(const LinearExpression& expression)
{
    if (!expression.constant().isInteger())
        return false;
    for (const auto& [variable, coefficient] : expression.terms())
        if (!integerDimension(variable) || !coefficient.isInteger())
            return false;
    return true;
}

LinearConstraint impossibleConstraint()
{
    return LinearConstraint(LinearExpression(Rational(1)),
                            ConstraintKind::LessEqual);
}

} // namespace

class ElinaOctagonDomain::Impl
{
public:
    explicit Impl(bool bottom)
        : manager_(makeManager()), state_(makeInitial(bottom))
    {
    }

    Impl(const Impl& other) : manager_(makeManager()), variables_(other.variables_)
    {
        ScopedElinaRounding rounding;
        resetResult(manager_);
        state_ = elina_abstract0_copy(manager_, other.state_);
        if (state_ == nullptr || !succeeded(manager_))
            throw std::runtime_error(exceptionReason(manager_, "copy"));
    }

    Impl& operator=(const Impl&) = delete;

    ~Impl()
    {
        if (state_ != nullptr)
        {
            try
            {
                ScopedElinaRounding rounding;
                elina_abstract0_free(manager_, state_);
            }
            catch (...)
            {
                // Destructors cannot recover from a changed process FPU.
            }
        }
        if (manager_ != nullptr)
            elina_manager_free(manager_);
    }

    elina_manager_t* manager() const
    {
        return manager_;
    }

    elina_abstract0_t* state() const
    {
        return state_;
    }

    std::size_t integerCount() const
    {
        return static_cast<std::size_t>(std::count_if(
            variables_.begin(), variables_.end(), integerDimension));
    }

    std::size_t realCount() const
    {
        return variables_.size() - integerCount();
    }

    bool contains(Variable variable) const
    {
        return std::find(variables_.begin(), variables_.end(), variable) !=
               variables_.end();
    }

    std::size_t dimensionOf(Variable variable) const
    {
        const auto found =
            std::find(variables_.begin(), variables_.end(), variable);
        if (found == variables_.end())
            throw std::out_of_range("missing ELINA Octagon coordinate");
        return static_cast<std::size_t>(found - variables_.begin());
    }

    Variable variableOf(std::size_t dimension) const
    {
        return variables_.at(dimension);
    }

    const std::vector<Variable>& variables() const
    {
        return variables_;
    }

    void eraseLogicalVariables(const std::vector<Variable>& removed)
    {
        for (Variable variable : removed)
            variables_.erase(std::find(variables_.begin(), variables_.end(),
                                       variable));
    }

    bool ensureVariables(const std::vector<Variable>& requested)
    {
        std::set<Variable> unique(requested.begin(), requested.end());
        for (Variable variable : unique)
        {
            if (contains(variable))
                continue;

            const std::size_t integers = integerCount();
            auto begin = integerDimension(variable) ? variables_.begin()
                         : variables_.begin() +
                               static_cast<std::ptrdiff_t>(integers);
            auto end = integerDimension(variable)
                       ? variables_.begin() +
                             static_cast<std::ptrdiff_t>(integers)
                       : variables_.end();
            const auto insertion = std::lower_bound(begin, end, variable);
            const std::size_t position = static_cast<std::size_t>(
                insertion - variables_.begin());

            elina_dimchange_t change;
            elina_dimchange_init(&change, integerDimension(variable) ? 1 : 0,
                                 integerDimension(variable) ? 0 : 1);
            change.dim[0] = static_cast<elina_dim_t>(position);
            ScopedElinaRounding rounding;
            resetResult(manager_);
            elina_abstract0_t* extended = elina_abstract0_add_dimensions(
                manager_, true, state_, &change, false);
            elina_dimchange_clear(&change);
            if (extended == nullptr || !succeeded(manager_))
            {
                if (extended != nullptr)
                    elina_abstract0_free(manager_, extended);
                return false;
            }
            replace(extended);
            variables_.insert(variables_.begin() +
                                  static_cast<std::ptrdiff_t>(position),
                              variable);
        }
        return true;
    }

    bool removeVariables(std::vector<Variable> removed)
    {
        std::sort(removed.begin(), removed.end(),
                  [this](Variable lhs, Variable rhs)
        {
            return dimensionOf(lhs) > dimensionOf(rhs);
        });
        for (Variable variable : removed)
        {
            if (!contains(variable))
                continue;
            const std::size_t dimension = dimensionOf(variable);
            elina_dimchange_t change;
            elina_dimchange_init(&change, integerDimension(variable) ? 1 : 0,
                                 integerDimension(variable) ? 0 : 1);
            change.dim[0] = static_cast<elina_dim_t>(dimension);
            ScopedElinaRounding rounding;
            resetResult(manager_);
            elina_abstract0_t* reduced = elina_abstract0_remove_dimensions(
                manager_, true, state_, &change);
            elina_dimchange_clear(&change);
            if (reduced == nullptr || !succeeded(manager_))
            {
                if (reduced != nullptr)
                    elina_abstract0_free(manager_, reduced);
                return false;
            }
            replace(reduced);
            variables_.erase(variables_.begin() +
                             static_cast<std::ptrdiff_t>(dimension));
        }
        return true;
    }

    elina_linexpr0_t* expression(const LinearExpression& expression) const
    {
        elina_linexpr0_t* result =
            elina_linexpr0_alloc(ELINA_LINEXPR_DENSE, variables_.size());
        elina_scalar_t* constant = rationalToScalar(expression.constant());
        elina_linexpr0_set_cst_scalar(result, constant);
        elina_scalar_free(constant);
        for (const auto& [variable, coefficient] : expression.terms())
        {
            elina_scalar_t* scalar = rationalToScalar(coefficient);
            const bool failed = elina_linexpr0_set_coeff_scalar(
                result, static_cast<elina_dim_t>(dimensionOf(variable)),
                scalar);
            elina_scalar_free(scalar);
            if (failed)
            {
                elina_linexpr0_free(result);
                throw std::invalid_argument(
                    "ELINA expression references an unknown dimension");
            }
        }
        return result;
    }

    void replace(elina_abstract0_t* replacement)
    {
        if (replacement == state_)
            return;
        elina_abstract0_free(manager_, state_);
        state_ = replacement;
    }

    void setTop()
    {
        ScopedElinaRounding rounding;
        resetResult(manager_);
        elina_abstract0_t* top = elina_abstract0_top(
            manager_, integerCount(), realCount());
        if (top == nullptr || !succeeded(manager_))
            throw std::runtime_error(exceptionReason(manager_, "top"));
        replace(top);
    }

private:
    static elina_manager_t* makeManager()
    {
        ScopedElinaRounding rounding;
        elina_manager_t* manager = opt_oct_manager_alloc();
        if (manager == nullptr)
            throw std::bad_alloc();
        if (manager->funptr[ELINA_FUNID_COPY] == nullptr ||
                manager->funptr[ELINA_FUNID_TOP] == nullptr ||
                manager->funptr[ELINA_FUNID_BOTTOM] == nullptr ||
                manager->funptr[ELINA_FUNID_SAT_LINCONS] == nullptr ||
                manager->funptr[ELINA_FUNID_BOUND_LINEXPR] == nullptr ||
                manager->funptr[ELINA_FUNID_TO_LINCONS_ARRAY] == nullptr ||
                manager->funptr[ELINA_FUNID_ASSIGN_LINEXPR_ARRAY] == nullptr ||
                manager->funptr[ELINA_FUNID_SUBSTITUTE_LINEXPR_ARRAY] == nullptr ||
                manager->funptr[ELINA_FUNID_MEET_LINCONS_ARRAY] == nullptr ||
                manager->funptr[ELINA_FUNID_JOIN] == nullptr ||
                manager->funptr[ELINA_FUNID_MEET] == nullptr ||
                manager->funptr[ELINA_FUNID_WIDENING] == nullptr ||
                manager->funptr[ELINA_FUNID_CLOSURE] == nullptr ||
                manager->funptr[ELINA_FUNID_ADD_DIMENSIONS] == nullptr ||
                manager->funptr[ELINA_FUNID_REMOVE_DIMENSIONS] == nullptr ||
                manager->funptr[ELINA_FUNID_FOLD] == nullptr)
        {
            elina_manager_free(manager);
            throw std::runtime_error(
                "fixed ELINA Octagon manager lacks required capabilities");
        }
        return manager;
    }

    elina_abstract0_t* makeInitial(bool bottom)
    {
        ScopedElinaRounding rounding;
        resetResult(manager_);
        elina_abstract0_t* initial = bottom
                                     ? elina_abstract0_bottom(manager_, 0, 0)
                                     : elina_abstract0_top(manager_, 0, 0);
        if (initial == nullptr || !succeeded(manager_))
            throw std::runtime_error(exceptionReason(
                manager_, bottom ? "bottom" : "top"));
        return initial;
    }

    elina_manager_t* manager_ = nullptr;
    elina_abstract0_t* state_ = nullptr;
    std::vector<Variable> variables_;
};

namespace
{

ApproximationKind approximation(elina_manager_t* manager)
{
    return manager->result.flag_exact ? ApproximationKind::Exact
           : ApproximationKind::SoundOverApproximation;
}

CheckResult predicateResult(elina_manager_t* manager, bool value)
{
    if (!succeeded(manager))
        return CheckResult::Unknown;
    if (value)
        return CheckResult::True;
    return manager->result.flag_exact ? CheckResult::False
           : CheckResult::Unknown;
}

struct PreparedConstraint
{
    elina_lincons0_t value;
    bool rewrittenStrictInteger = false;
};

PreparedConstraint prepareConstraint(ElinaOctagonDomain::Impl& impl,
                                     const LinearConstraint& constraint,
                                     bool integerTightening)
{
    LinearExpression expression = constraint.expression();
    ConstraintKind kind = constraint.kind();
    if (kind == ConstraintKind::LessEqual ||
            kind == ConstraintKind::LessThan)
    {
        expression = -expression;
        kind = kind == ConstraintKind::LessEqual
               ? ConstraintKind::GreaterEqual : ConstraintKind::GreaterThan;
    }

    bool rewritten = false;
    if (kind == ConstraintKind::GreaterThan && integerTightening &&
            integralExpression(expression))
    {
        expression.setConstant(expression.constant() - Rational(1));
        kind = ConstraintKind::GreaterEqual;
        rewritten = true;
    }

    elina_constyp_t elinaKind = ELINA_CONS_EQ;
    switch (kind)
    {
    case ConstraintKind::Equal:
        elinaKind = ELINA_CONS_EQ;
        break;
    case ConstraintKind::NotEqual:
        elinaKind = ELINA_CONS_DISEQ;
        break;
    case ConstraintKind::GreaterEqual:
        elinaKind = ELINA_CONS_SUPEQ;
        break;
    case ConstraintKind::GreaterThan:
        elinaKind = ELINA_CONS_SUP;
        break;
    case ConstraintKind::LessEqual:
    case ConstraintKind::LessThan:
        throw std::logic_error("constraint direction was not normalized");
    }
    return {elina_lincons0_make(elinaKind, impl.expression(expression), nullptr),
            rewritten};
}

std::vector<Variable> expressionVariables(const LinearExpression& expression)
{
    std::vector<Variable> variables;
    variables.reserve(expression.terms().size());
    for (const auto& [variable, coefficient] : expression.terms())
    {
        (void)coefficient;
        variables.push_back(variable);
    }
    return variables;
}

} // namespace

ElinaOctagonCapabilities ElinaOctagonDomain::capabilities() noexcept
{
    return {};
}

bool ElinaOctagonDomain::runtimeFpuSupported() noexcept
{
    return fpuRoundTrip();
}

ElinaOctagonDomain::ElinaOctagonDomain(OctagonConfig config, bool bottom)
    : config_(std::move(config)), impl_(std::make_unique<Impl>(bottom))
{
}

ElinaOctagonDomain::ElinaOctagonDomain(OctagonConfig config,
                                       std::unique_ptr<Impl> impl)
    : config_(std::move(config)), impl_(std::move(impl))
{
}

ElinaOctagonDomain ElinaOctagonDomain::top(const OctagonConfig& config)
{
    return ElinaOctagonDomain(config, false);
}

ElinaOctagonDomain ElinaOctagonDomain::bottom(const OctagonConfig& config)
{
    return ElinaOctagonDomain(config, true);
}

ElinaOctagonDomain ElinaOctagonDomain::fromConstraints(
    const LinearConstraintSet& constraints, const OctagonConfig& config)
{
    ElinaOctagonDomain result = top(config);
    result.assumeAll(constraints);
    return result;
}

ElinaOctagonDomain::ElinaOctagonDomain(const ElinaOctagonDomain& other)
    : NumericalDomain(other), config_(other.config_),
      impl_(std::make_unique<Impl>(*other.impl_))
{
}

ElinaOctagonDomain::ElinaOctagonDomain(ElinaOctagonDomain&& other) noexcept =
    default;

ElinaOctagonDomain& ElinaOctagonDomain::operator=(
    const ElinaOctagonDomain& other)
{
    if (this == &other)
        return *this;
    NumericalDomain::operator=(other);
    config_ = other.config_;
    impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}

ElinaOctagonDomain& ElinaOctagonDomain::operator=(
    ElinaOctagonDomain&& other) noexcept = default;

ElinaOctagonDomain::~ElinaOctagonDomain() = default;

std::unique_ptr<AbstractDomain> ElinaOctagonDomain::clone() const
{
    return std::make_unique<ElinaOctagonDomain>(*this);
}

const char* ElinaOctagonDomain::name() const noexcept
{
    return "ELINA Octagon f524156d";
}

const OctagonConfig& ElinaOctagonDomain::config() const noexcept
{
    return config_;
}

void ElinaOctagonDomain::report(OperationKind operation,
                                ApproximationKind kind, std::string reason,
                                bool best) const
{
    recordOperation(operation, kind, best, reason);
    if (config_.diagnostics && kind != ApproximationKind::Exact)
        config_.diagnostics->report({operation, kind, std::move(reason)});
}

void ElinaOctagonDomain::assign(Variable target,
                                const LinearExpression& expression)
{
    std::vector<Variable> variables = expressionVariables(expression);
    variables.push_back(target);
    if (!impl_->ensureVariables(variables))
    {
        forget(target);
        report(OperationKind::Assignment,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "dimension extension"), false);
        return;
    }

    elina_linexpr0_t* converted = impl_->expression(expression);
    ScopedElinaRounding rounding;
    resetResult(impl_->manager());
    elina_abstract0_t* assigned = elina_abstract0_assign_linexpr(
        impl_->manager(), true, impl_->state(),
        static_cast<elina_dim_t>(impl_->dimensionOf(target)), converted,
        nullptr);
    elina_linexpr0_free(converted);
    if (assigned == nullptr || !succeeded(impl_->manager()))
    {
        if (assigned != nullptr)
            elina_abstract0_free(impl_->manager(), assigned);
        const std::string reason =
            exceptionReason(impl_->manager(), "linear assignment");
        forget(target);
        report(OperationKind::Assignment,
               ApproximationKind::UnsupportedFallback, reason, false);
        return;
    }
    const ApproximationKind kind = approximation(impl_->manager());
    const bool best = impl_->manager()->result.flag_best;
    impl_->replace(assigned);
    report(OperationKind::Assignment, kind,
           kind == ApproximationKind::Exact ? std::string() :
           "ELINA Octagon rounded or approximated a linear assignment",
           best);
}

void ElinaOctagonDomain::assign(Variable target,
                                const TreeExpression& expression)
{
    if (const auto linear = expression.asLinear())
    {
        assign(target, *linear);
        return;
    }
    assignInterval(target, evaluateTreeExpression(expression));
    report(OperationKind::Assignment,
           ApproximationKind::SoundOverApproximation,
           "ELINA Octagon interval-linearized a nonlinear or IEEE assignment",
           false);
}

void ElinaOctagonDomain::assignParallel(
    const LinearAssignmentList& assignments)
{
    if (assignments.empty())
        return;
    std::set<Variable> targets;
    std::vector<Variable> variables;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument("duplicate parallel target");
        variables.push_back(assignment.target);
        const auto used = expressionVariables(assignment.expression);
        variables.insert(variables.end(), used.begin(), used.end());
    }
    if (!impl_->ensureVariables(variables))
    {
        for (Variable target : targets)
            forget(target);
        report(OperationKind::Assignment,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "dimension extension"), false);
        return;
    }

    // The fixed opt_oct destructive multi-assignment corrupts its internal
    // matrix before a later dimension removal. Preserve simultaneous
    // semantics with fresh coordinates: all right-hand sides first read the
    // unchanged source variables, then targets read only those snapshots.
    std::set<Variable> occupied(impl_->variables().begin(),
                                impl_->variables().end());
    std::vector<Variable> temporaries;
    temporaries.reserve(assignments.size());
    for (const LinearAssignment& assignment : assignments)
    {
        std::uint64_t id = 0;
        while (id <= std::numeric_limits<std::uint32_t>::max() &&
                occupied.count(Variable(static_cast<std::uint32_t>(id),
                                        assignment.target.type())) != 0)
            ++id;
        if (id > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error(
                "no temporary variable available for parallel assignment");
        Variable temporary(static_cast<std::uint32_t>(id),
                           assignment.target.type());
        temporaries.push_back(temporary);
        occupied.insert(temporary);
    }

    bool usedFallback = false;
    for (std::size_t index = 0; index < assignments.size(); ++index)
    {
        assign(temporaries[index], assignments[index].expression);
        usedFallback = usedFallback ||
                       lastOperation().approximation ==
                           ApproximationKind::UnsupportedFallback;
    }
    for (std::size_t index = 0; index < assignments.size(); ++index)
    {
        assign(assignments[index].target,
               LinearExpression(temporaries[index]));
        usedFallback = usedFallback ||
                       lastOperation().approximation ==
                           ApproximationKind::UnsupportedFallback;
    }
    for (Variable temporary : temporaries)
    {
        forget(temporary);
        usedFallback = usedFallback ||
                       lastOperation().approximation ==
                           ApproximationKind::UnsupportedFallback;
    }
    report(OperationKind::Assignment,
           usedFallback ? ApproximationKind::UnsupportedFallback
                        : ApproximationKind::SoundOverApproximation,
           usedFallback
               ? "ELINA Octagon parallel snapshot used a conservative "
                 "fallback"
               : "ELINA f524156d parallel assignment staged through fresh "
                 "coordinates because its direct destructive operation "
                 "corrupts later dimension removal",
           false);
}

void ElinaOctagonDomain::substitute(
    Variable target, const LinearExpression& expression)
{
    substituteParallel({{target, expression}});
}

void ElinaOctagonDomain::substituteParallel(
    const LinearAssignmentList& assignments)
{
    if (assignments.empty())
        return;
    std::set<Variable> targets;
    std::vector<Variable> variables;
    for (const LinearAssignment& assignment : assignments)
    {
        if (!targets.insert(assignment.target).second)
            throw std::invalid_argument("duplicate parallel target");
        variables.push_back(assignment.target);
        const auto used = expressionVariables(assignment.expression);
        variables.insert(variables.end(), used.begin(), used.end());
    }
    if (!impl_->ensureVariables(variables))
    {
        impl_->setTop();
        report(OperationKind::Substitution,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "dimension extension"), false);
        return;
    }

    std::vector<elina_dim_t> dimensions;
    std::vector<elina_linexpr0_t*> expressions;
    for (const LinearAssignment& assignment : assignments)
    {
        dimensions.push_back(static_cast<elina_dim_t>(
            impl_->dimensionOf(assignment.target)));
        expressions.push_back(impl_->expression(assignment.expression));
    }
    ScopedElinaRounding rounding;
    resetResult(impl_->manager());
    elina_abstract0_t* substituted = elina_abstract0_substitute_linexpr_array(
        impl_->manager(), true, impl_->state(), dimensions.data(),
        expressions.data(), expressions.size(), nullptr);
    for (elina_linexpr0_t* expression : expressions)
        elina_linexpr0_free(expression);
    if (substituted == nullptr || !succeeded(impl_->manager()))
    {
        if (substituted != nullptr)
            elina_abstract0_free(impl_->manager(), substituted);
        const std::string reason =
            exceptionReason(impl_->manager(), "parallel substitution");
        impl_->setTop();
        report(OperationKind::Substitution,
               ApproximationKind::UnsupportedFallback, reason, false);
        return;
    }
    const ApproximationKind kind = approximation(impl_->manager());
    const bool best = impl_->manager()->result.flag_best;
    impl_->replace(substituted);
    report(OperationKind::Substitution, kind,
           kind == ApproximationKind::Exact ? std::string() :
           "ELINA Octagon approximated a substitution",
           best);
}

void ElinaOctagonDomain::assume(const LinearConstraint& constraint)
{
    const auto variables = expressionVariables(constraint.expression());
    if (!impl_->ensureVariables(variables))
    {
        report(OperationKind::Assumption,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "dimension extension"), false);
        return;
    }
    PreparedConstraint converted = prepareConstraint(
        *impl_, constraint, config_.integerTightening);
    elina_lincons0_array_t array = elina_lincons0_array_make(1);
    array.p[0] = converted.value;
    ScopedElinaRounding rounding;
    resetResult(impl_->manager());
    elina_abstract0_t* assumed = elina_abstract0_meet_lincons_array(
        impl_->manager(), true, impl_->state(), &array);
    elina_lincons0_array_clear(&array);
    if (assumed == nullptr || !succeeded(impl_->manager()))
    {
        if (assumed != nullptr)
            elina_abstract0_free(impl_->manager(), assumed);
        report(OperationKind::Assumption,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "linear assumption"), false);
        return;
    }
    const ApproximationKind kind = approximation(impl_->manager());
    const bool best = impl_->manager()->result.flag_best;
    impl_->replace(assumed);
    report(OperationKind::Assumption, kind,
           kind == ApproximationKind::Exact ? std::string() :
           "ELINA Octagon rounded or approximated a linear assumption",
           best);
}

void ElinaOctagonDomain::assumeAll(
    const LinearConstraintSet& constraints)
{
    for (const LinearConstraint& constraint : constraints)
    {
        assume(constraint);
        if (isBottom())
            return;
    }
}

void ElinaOctagonDomain::assume(const TreeConstraint& constraint)
{
    if (const auto linear = constraint.expression().asLinear())
    {
        assume(LinearConstraint(*linear, constraint.kind()));
        return;
    }
    assumeAll(treeConstraintConsequences(constraint));
    report(OperationKind::Assumption,
           ApproximationKind::SoundOverApproximation,
           "ELINA Octagon used necessary affine consequences of a tree guard",
           false);
}

void ElinaOctagonDomain::forget(Variable variable)
{
    if (!impl_->contains(variable))
    {
        report(OperationKind::Forget, ApproximationKind::Exact);
        return;
    }
    if (!impl_->removeVariables({variable}))
    {
        report(OperationKind::Forget,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "forget/remove dimension"),
               false);
        return;
    }
    report(OperationKind::Forget, ApproximationKind::Exact);
}

void ElinaOctagonDomain::project(const std::vector<Variable>& retained)
{
    const std::set<Variable> keep(retained.begin(), retained.end());
    std::vector<Variable> removed;
    for (Variable variable : impl_->variables())
        if (keep.count(variable) == 0)
            removed.push_back(variable);
    if (!impl_->removeVariables(removed))
        report(OperationKind::Forget,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "projection"), false);
    else
        report(OperationKind::Forget, ApproximationKind::Exact);
}

void ElinaOctagonDomain::expand(Variable source,
                                const std::vector<Variable>& copies)
{
    if (copies.empty())
        return;
    std::set<Variable> unique(copies.begin(), copies.end());
    if (unique.size() != copies.size() || unique.count(source) != 0)
        throw std::invalid_argument("invalid ELINA Octagon expansion set");
    for (Variable copy : copies)
    {
        if (copy.type() != source.type())
            throw std::invalid_argument(
                "ELINA Octagon expansion requires same-type copies");
        if (impl_->contains(copy))
            throw std::invalid_argument("expansion target already exists");
    }
    if (!impl_->contains(source))
    {
        std::vector<Variable> all = copies;
        all.push_back(source);
        if (!impl_->ensureVariables(all))
        {
            report(OperationKind::Expand,
                   ApproximationKind::UnsupportedFallback,
                   exceptionReason(impl_->manager(), "expansion dimensions"),
                   false);
            return;
        }
        report(OperationKind::Expand, ApproximationKind::Exact);
        return;
    }

    const LinearConstraintSet original = toConstraints();
    if (!impl_->ensureVariables(copies))
    {
        report(OperationKind::Expand,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "expansion dimensions"),
               false);
        return;
    }
    for (Variable copy : copies)
    {
        std::map<Variable, LinearExpression> replacement;
        replacement.emplace(source, LinearExpression(copy));
        for (const LinearConstraint& constraint : original)
            assume(LinearConstraint(
                constraint.expression().substituted(replacement),
                constraint.kind()));
    }
    report(OperationKind::Expand,
           ApproximationKind::SoundOverApproximation,
           "ELINA Octagon expanded through exported linear consequences",
           false);
}

void ElinaOctagonDomain::fold(Variable target,
                              const std::vector<Variable>& folded)
{
    std::vector<Variable> dimensions = folded;
    dimensions.erase(std::remove(dimensions.begin(), dimensions.end(), target),
                     dimensions.end());
    if (dimensions.empty())
        return;
    std::set<Variable> unique(dimensions.begin(), dimensions.end());
    if (unique.size() != dimensions.size())
        throw std::invalid_argument("duplicate ELINA Octagon fold dimension");
    for (Variable variable : dimensions)
        if (variable.type() != target.type())
            throw std::invalid_argument(
                "ELINA Octagon fold requires same-type dimensions");
    std::vector<Variable> required = dimensions;
    required.push_back(target);
    if (!impl_->ensureVariables(required))
    {
        impl_->setTop();
        report(OperationKind::Fold,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "fold dimensions"), false);
        return;
    }
    std::vector<elina_dim_t> physical;
    physical.push_back(
        static_cast<elina_dim_t>(impl_->dimensionOf(target)));
    for (Variable variable : dimensions)
        physical.push_back(
            static_cast<elina_dim_t>(impl_->dimensionOf(variable)));
    ScopedElinaRounding rounding;
    resetResult(impl_->manager());
    elina_abstract0_t* result = elina_abstract0_fold(
        impl_->manager(), true, impl_->state(), physical.data(),
        physical.size());
    if (result == nullptr || !succeeded(impl_->manager()))
    {
        if (result != nullptr)
            elina_abstract0_free(impl_->manager(), result);
        const std::string reason = exceptionReason(impl_->manager(), "fold");
        impl_->setTop();
        report(OperationKind::Fold,
               ApproximationKind::UnsupportedFallback, reason, false);
        return;
    }
    const ApproximationKind kind = approximation(impl_->manager());
    const bool best = impl_->manager()->result.flag_best;
    impl_->replace(result);
    // ELINA removed the folded coordinates; update only the logical layout.
    impl_->eraseLogicalVariables(dimensions);
    report(OperationKind::Fold, kind,
           kind == ApproximationKind::Exact ? std::string() :
           "ELINA Octagon approximated a fold",
           best);
}

CheckResult ElinaOctagonDomain::entails(
    const LinearConstraint& constraint) const
{
    ElinaOctagonDomain aligned(*this);
    if (!aligned.impl_->ensureVariables(
            expressionVariables(constraint.expression())))
        return CheckResult::Unknown;
    PreparedConstraint converted = prepareConstraint(
        *aligned.impl_, constraint, config_.integerTightening);
    ScopedElinaRounding rounding;
    resetResult(aligned.impl_->manager());
    const bool result = elina_abstract0_sat_lincons(
        aligned.impl_->manager(), aligned.impl_->state(), &converted.value);
    elina_lincons0_clear(&converted.value);
    return predicateResult(aligned.impl_->manager(), result);
}

CheckResult ElinaOctagonDomain::subsetOf(
    const ElinaOctagonDomain& other) const
{
    requireCompatible(other);
    ElinaOctagonDomain lhs(*this);
    ElinaOctagonDomain rhs(other);
    std::vector<Variable> all = lhs.impl_->variables();
    all.insert(all.end(), rhs.impl_->variables().begin(),
               rhs.impl_->variables().end());
    if (!lhs.impl_->ensureVariables(all) ||
            !rhs.impl_->ensureVariables(all))
        return CheckResult::Unknown;
    ScopedElinaRounding rounding;
    resetResult(lhs.impl_->manager());
    const bool result = elina_abstract0_is_leq(
        lhs.impl_->manager(), lhs.impl_->state(), rhs.impl_->state());
    return predicateResult(lhs.impl_->manager(), result);
}

Interval ElinaOctagonDomain::bound(Variable variable) const
{
    // Bottom denotes no concrete valuation, including for variables that do
    // not yet have a physical ELINA coordinate.
    if (isBottom())
        return Interval::bottom();
    if (!impl_->contains(variable))
        return Interval::top();
    ScopedElinaRounding rounding;
    resetResult(impl_->manager());
    elina_interval_t* result = elina_abstract0_bound_dimension(
        impl_->manager(), impl_->state(),
        static_cast<elina_dim_t>(impl_->dimensionOf(variable)));
    if (result == nullptr || !succeeded(impl_->manager()))
    {
        if (result != nullptr)
            elina_interval_free(result);
        return Interval::top();
    }
    const Interval interval(lowerBound(result->inf), upperBound(result->sup));
    elina_interval_free(result);
    return interval;
}

Interval ElinaOctagonDomain::bound(
    const LinearExpression& expression) const
{
    if (isBottom())
        return Interval::bottom();
    ElinaOctagonDomain aligned(*this);
    if (!aligned.impl_->ensureVariables(expressionVariables(expression)))
        return Interval::top();
    elina_linexpr0_t* converted = aligned.impl_->expression(expression);
    ScopedElinaRounding rounding;
    resetResult(aligned.impl_->manager());
    elina_interval_t* result = elina_abstract0_bound_linexpr(
        aligned.impl_->manager(), aligned.impl_->state(), converted);
    elina_linexpr0_free(converted);
    if (result == nullptr || !succeeded(aligned.impl_->manager()))
    {
        if (result != nullptr)
            elina_interval_free(result);
        return Interval::top();
    }
    const Interval interval(lowerBound(result->inf), upperBound(result->sup));
    elina_interval_free(result);
    return interval;
}

std::vector<Variable> ElinaOctagonDomain::supportVariables() const
{
    std::vector<Variable> result;
    for (Variable variable : impl_->variables())
    {
        ScopedElinaRounding rounding;
        resetResult(impl_->manager());
        const bool unconstrained = elina_abstract0_is_dimension_unconstrained(
            impl_->manager(), impl_->state(),
            static_cast<elina_dim_t>(impl_->dimensionOf(variable)));
        const CheckResult query =
            predicateResult(impl_->manager(), unconstrained);
        if (query != CheckResult::True)
            result.push_back(variable);
    }
    std::sort(result.begin(), result.end());
    return result;
}

LinearConstraintSet ElinaOctagonDomain::toConstraints() const
{
    ScopedElinaRounding rounding;
    resetResult(impl_->manager());
    elina_lincons0_array_t constraints = elina_abstract0_to_lincons_array(
        impl_->manager(), impl_->state());
    LinearConstraintSet result;
    if (!succeeded(impl_->manager()))
    {
        elina_lincons0_array_clear(&constraints);
        return result;
    }
    result.reserve(constraints.size);
    for (std::size_t index = 0; index < constraints.size; ++index)
    {
        elina_lincons0_t& source = constraints.p[index];
        LinearExpression expression;
        const auto constant = coefficientValue(&source.linexpr0->cst);
        if (!constant)
            continue;
        expression.setConstant(*constant);
        bool representable = true;
        for (std::size_t dimension = 0;
                dimension < impl_->variables().size(); ++dimension)
        {
            elina_coeff_t* coefficient = elina_linexpr0_coeffref(
                source.linexpr0, static_cast<elina_dim_t>(dimension));
            if (coefficient == nullptr)
                continue;
            const auto value = coefficientValue(coefficient);
            if (!value)
            {
                representable = false;
                break;
            }
            if (!value->isZero())
                expression.setCoefficient(impl_->variableOf(dimension), *value);
        }
        if (!representable)
            continue;
        switch (source.constyp)
        {
        case ELINA_CONS_EQ:
            result.emplace_back(std::move(expression), ConstraintKind::Equal);
            break;
        case ELINA_CONS_SUPEQ:
            result.emplace_back(std::move(expression),
                                ConstraintKind::GreaterEqual);
            break;
        case ELINA_CONS_SUP:
            result.emplace_back(std::move(expression),
                                ConstraintKind::GreaterThan);
            break;
        case ELINA_CONS_DISEQ:
            result.emplace_back(std::move(expression),
                                ConstraintKind::NotEqual);
            break;
        case ELINA_CONS_EQMOD:
            break;
        }
    }
    elina_lincons0_array_clear(&constraints);
    if (result.empty() && isBottom())
        result.push_back(impossibleConstraint());
    return result;
}

void ElinaOctagonDomain::close()
{
    ScopedElinaRounding rounding;
    resetResult(impl_->manager());
    elina_abstract0_t* closed = elina_abstract0_closure(
        impl_->manager(), true, impl_->state());
    if (closed == nullptr || !succeeded(impl_->manager()))
    {
        if (closed != nullptr)
            elina_abstract0_free(impl_->manager(), closed);
        report(OperationKind::TopologicalClosure,
               ApproximationKind::UnsupportedFallback,
               exceptionReason(impl_->manager(), "topological closure"),
               false);
        return;
    }
    const ApproximationKind kind = approximation(impl_->manager());
    const bool best = impl_->manager()->result.flag_best;
    impl_->replace(closed);
    report(OperationKind::TopologicalClosure, kind,
           kind == ApproximationKind::Exact ? std::string() :
           "ELINA Octagon approximated topological closure",
           best);
}

void ElinaOctagonDomain::canonicalize()
{
    // Fixed-version source registers this callback but implements it solely by
    // raising NOT_IMPLEMENTED. Do not probe a known unsupported operation.
    report(OperationKind::Canonicalization,
           ApproximationKind::UnsupportedFallback,
           "ELINA f524156d Octagon canonicalization/minimize is unsupported",
           false);
}

ElinaOctagonDomain ElinaOctagonDomain::join(
    const ElinaOctagonDomain& other) const
{
    requireCompatible(other);
    ElinaOctagonDomain lhs(*this);
    ElinaOctagonDomain rhs(other);
    std::vector<Variable> all = lhs.impl_->variables();
    all.insert(all.end(), rhs.impl_->variables().begin(),
               rhs.impl_->variables().end());
    if (!lhs.impl_->ensureVariables(all) || !rhs.impl_->ensureVariables(all))
    {
        lhs.impl_->setTop();
        lhs.report(OperationKind::Join,
                   ApproximationKind::UnsupportedFallback,
                   "ELINA Octagon could not align join dimensions", false);
        return lhs;
    }
    ScopedElinaRounding rounding;
    resetResult(lhs.impl_->manager());
    elina_abstract0_t* joined = elina_abstract0_join(
        lhs.impl_->manager(), false, lhs.impl_->state(), rhs.impl_->state());
    if (joined == nullptr || !succeeded(lhs.impl_->manager()))
    {
        if (joined != nullptr)
            elina_abstract0_free(lhs.impl_->manager(), joined);
        const std::string reason =
            exceptionReason(lhs.impl_->manager(), "join");
        lhs.impl_->setTop();
        lhs.report(OperationKind::Join,
                   ApproximationKind::UnsupportedFallback, reason, false);
        return lhs;
    }
    const ApproximationKind kind = approximation(lhs.impl_->manager());
    const bool best = lhs.impl_->manager()->result.flag_best;
    lhs.impl_->replace(joined);
    lhs.report(OperationKind::Join, kind,
               kind == ApproximationKind::Exact ? std::string() :
               "ELINA Octagon approximated join",
               best);
    return lhs;
}

ElinaOctagonDomain ElinaOctagonDomain::meet(
    const ElinaOctagonDomain& other) const
{
    requireCompatible(other);
    ElinaOctagonDomain lhs(*this);
    ElinaOctagonDomain rhs(other);
    std::vector<Variable> all = lhs.impl_->variables();
    all.insert(all.end(), rhs.impl_->variables().begin(),
               rhs.impl_->variables().end());
    if (!lhs.impl_->ensureVariables(all) || !rhs.impl_->ensureVariables(all))
    {
        lhs.report(OperationKind::Meet,
                   ApproximationKind::UnsupportedFallback,
                   "ELINA Octagon could not align meet dimensions", false);
        return lhs;
    }
    ScopedElinaRounding rounding;
    resetResult(lhs.impl_->manager());
    elina_abstract0_t* met = elina_abstract0_meet(
        lhs.impl_->manager(), false, lhs.impl_->state(), rhs.impl_->state());
    if (met == nullptr || !succeeded(lhs.impl_->manager()))
    {
        if (met != nullptr)
            elina_abstract0_free(lhs.impl_->manager(), met);
        lhs.report(OperationKind::Meet,
                   ApproximationKind::UnsupportedFallback,
                   exceptionReason(lhs.impl_->manager(), "meet"), false);
        return lhs;
    }
    const ApproximationKind kind = approximation(lhs.impl_->manager());
    const bool best = lhs.impl_->manager()->result.flag_best;
    lhs.impl_->replace(met);
    lhs.report(OperationKind::Meet, kind,
               kind == ApproximationKind::Exact ? std::string() :
               "ELINA Octagon approximated meet",
               best);
    return lhs;
}

ElinaOctagonDomain ElinaOctagonDomain::widen(
    const ElinaOctagonDomain& next, const WideningPolicy& policy) const
{
    requireCompatible(next);
    ElinaOctagonDomain current(*this);
    ElinaOctagonDomain successor(next);
    std::vector<Variable> all = current.impl_->variables();
    all.insert(all.end(), successor.impl_->variables().begin(),
               successor.impl_->variables().end());
    if (!current.impl_->ensureVariables(all) ||
            !successor.impl_->ensureVariables(all))
    {
        current.impl_->setTop();
        current.report(OperationKind::Widening,
                       ApproximationKind::UnsupportedFallback,
                       "ELINA Octagon could not align widening dimensions",
                       false);
        return current;
    }
    ScopedElinaRounding rounding;
    resetResult(current.impl_->manager());
    elina_abstract0_t* widened = elina_abstract0_widening(
        current.impl_->manager(), current.impl_->state(),
        successor.impl_->state());
    if (widened == nullptr || !succeeded(current.impl_->manager()))
    {
        if (widened != nullptr)
            elina_abstract0_free(current.impl_->manager(), widened);
        const std::string reason =
            exceptionReason(current.impl_->manager(), "widening");
        current.impl_->setTop();
        current.report(OperationKind::Widening,
                       ApproximationKind::UnsupportedFallback, reason, false);
        return current;
    }
    const ApproximationKind kind = approximation(current.impl_->manager());
    const bool best = current.impl_->manager()->result.flag_best;
    current.impl_->replace(widened);
    const bool thresholds = !policy.thresholds.empty() ||
                            !policy.linearThresholds.empty();
    current.report(OperationKind::Widening,
                   thresholds ? ApproximationKind::UnsupportedFallback
                   : kind,
                   thresholds
                   ? "ELINA Octagon lacks threshold widening; used ordinary widening"
                   : (kind == ApproximationKind::Exact
                      ? std::string() :
                      "ELINA Octagon widening is an over-approximation"),
                   !thresholds && best);
    return current;
}

ElinaOctagonDomain ElinaOctagonDomain::narrow(
    const ElinaOctagonDomain& next) const
{
    ElinaOctagonDomain result = meet(next);
    result.report(OperationKind::Narrowing,
                  ApproximationKind::UnsupportedFallback,
                  "ELINA f524156d has no narrowing API; used sound meet fallback",
                  false);
    return result;
}

void ElinaOctagonDomain::assignInterval(Variable target,
                                        const Interval& value)
{
    forget(target);
    if (value.isBottom())
    {
        assume(impossibleConstraint());
        return;
    }
    if (value.lower().isFinite())
        assume(LinearConstraint(
            LinearExpression(target) -
                LinearExpression(value.lower().value()),
            value.lower().isStrict() ? ConstraintKind::GreaterThan
                                     : ConstraintKind::GreaterEqual));
    if (value.upper().isFinite())
        assume(LinearConstraint(
            LinearExpression(target) -
                LinearExpression(value.upper().value()),
            value.upper().isStrict() ? ConstraintKind::LessThan
                                     : ConstraintKind::LessEqual));
}

bool ElinaOctagonDomain::hasCompatibleDomain(
    const AbstractDomain& other) const
{
    return other.isDomain<ElinaOctagonDomain>() &&
           config_.operationCompatible(
               static_cast<const ElinaOctagonDomain&>(other).config_);
}

const ElinaOctagonDomain& ElinaOctagonDomain::requireElina(
    const AbstractDomain& other) const
{
    requireCompatible(other);
    return static_cast<const ElinaOctagonDomain&>(other);
}

void ElinaOctagonDomain::joinDomain(const AbstractDomain& other)
{
    *this = join(requireElina(other));
}

void ElinaOctagonDomain::meetDomain(const AbstractDomain& other)
{
    *this = meet(requireElina(other));
}

void ElinaOctagonDomain::widenDomain(const AbstractDomain& next)
{
    *this = widen(requireElina(next));
}

void ElinaOctagonDomain::narrowDomain(const AbstractDomain& next)
{
    *this = narrow(requireElina(next));
}

bool ElinaOctagonDomain::isBottomDomain() const
{
    ScopedElinaRounding rounding;
    resetResult(impl_->manager());
    const bool result =
        elina_abstract0_is_bottom(impl_->manager(), impl_->state());
    return succeeded(impl_->manager()) && result;
}

bool ElinaOctagonDomain::isTopDomain() const
{
    ScopedElinaRounding rounding;
    resetResult(impl_->manager());
    const bool result =
        elina_abstract0_is_top(impl_->manager(), impl_->state());
    return succeeded(impl_->manager()) && result;
}

bool ElinaOctagonDomain::leqDomain(const AbstractDomain& other) const
{
    return subsetOf(requireElina(other)) == CheckResult::True;
}

std::string ElinaOctagonDomain::domainToString() const
{
    if (isBottom())
        return "ELINAOctagon(bottom)";
    const LinearConstraintSet constraints = toConstraints();
    if (constraints.empty())
        return "ELINAOctagon(top)";
    std::ostringstream output;
    output << "ELINAOctagon{";
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
