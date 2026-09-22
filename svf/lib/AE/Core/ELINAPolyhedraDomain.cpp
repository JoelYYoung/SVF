//===- ELINAPolyhedraDomain.cpp -- ELINA Polyhedra adapter --------===//

#include "AE/Core/ELINAPolyhedraDomain.h"

extern "C"
{
#include "elina_abstract0.h"
#include "opt_pk.h"
}

#include <algorithm>
#include <cfenv>
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
        std::fesetround(previous_);
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

std::vector<Variable> orderedVariables(const ConvexPolyhedraDomain& state)
{
    std::vector<Variable> integers;
    std::vector<Variable> reals;
    for (Variable variable : state.supportVariables())
    {
        (variable.type().kind == NumericKind::Integer ? integers : reals)
            .push_back(variable);
    }
    integers.insert(integers.end(), reals.begin(), reals.end());
    return integers;
}

std::size_t integerDimensions(const std::vector<Variable>& variables)
{
    return static_cast<std::size_t>(std::count_if(
        variables.begin(), variables.end(), [](Variable variable)
        { return variable.type().kind == NumericKind::Integer; }));
}

std::size_t dimensionOf(const std::vector<Variable>& variables,
                        Variable variable)
{
    const auto found = std::find(variables.begin(), variables.end(), variable);
    if (found == variables.end())
        throw std::logic_error("ELINA constraint variable missing from layout");
    return static_cast<std::size_t>(found - variables.begin());
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
                input.expression(),
                input.kind() == ConstraintKind::LessThan
                    ? ConstraintKind::LessEqual
                    : ConstraintKind::GreaterEqual);
        }
    }

    LinearExpression expression = constraint.expression();
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
    return elina_lincons0_make(type, result, nullptr);
}

} // namespace

class ELINAPolyhedraDomain::Impl
{
public:
    Impl()
    {
        const int previous = std::fegetround();
        manager = opt_pk_manager_alloc(false);
        std::fesetround(previous);
        if (!manager)
            throw std::runtime_error("ELINA Polyhedra manager allocation failed");
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

    void synchronize(const ConvexPolyhedraDomain& native)
    {
        ELINARoundingScope rounding;
        resetResult(manager);
        if (state)
        {
            elina_abstract0_free(manager, state);
            state = nullptr;
        }
        variables = orderedVariables(native);
        const std::size_t integers = integerDimensions(variables);
        const std::size_t reals = variables.size() - integers;
        state = native.isBottom()
                    ? elina_abstract0_bottom(manager, integers, reals)
                    : elina_abstract0_top(manager, integers, reals);
        usable = state != nullptr && manager->result.exn == ELINA_EXC_NONE;
        approximated = false;
        if (!usable || native.isBottom())
            return;

        const LinearConstraintSet constraints = native.toConstraints();
        elina_lincons0_array_t array =
            elina_lincons0_array_make(constraints.size());
        for (std::size_t index = 0; index < constraints.size(); ++index)
            array.p[index] = makeConstraint(constraints[index], variables,
                                            approximated);
        resetResult(manager);
        elina_abstract0_t* constrained = elina_abstract0_meet_lincons_array(
            manager, false, state, &array);
        elina_lincons0_array_clear(&array);
        if (constrained == nullptr || manager->result.exn != ELINA_EXC_NONE)
        {
            if (constrained)
                elina_abstract0_free(manager, constrained);
            usable = false;
            return;
        }
        elina_abstract0_free(manager, state);
        state = constrained;
        usable = true;
    }

    CheckResult inclusion(const Impl& other) const
    {
        if (!usable || !other.usable || variables != other.variables)
            return CheckResult::Unknown;
        ELINARoundingScope rounding;
        resetResult(manager);
        const bool result = elina_abstract0_is_leq(
            manager, state, other.state);
        return classifyELINAPredicate(
            result, manager->result.flag_exact, manager->result.flag_best,
            manager->result.exn != ELINA_EXC_NONE);
    }

    elina_manager_t* manager = nullptr;
    elina_abstract0_t* state = nullptr;
    std::vector<Variable> variables;
    bool usable = false;
    bool approximated = false;
};

ELINAPolyhedraDomain::ELINAPolyhedraDomain(ELINAPolyhedraConfig config,
                                           bool bottom)
    : config_(std::move(config)),
      native_(bottom ? ConvexPolyhedraDomain::bottom(
                           {config_.diagnostics, config_.integerTightening})
                     : ConvexPolyhedraDomain::top(
                           {config_.diagnostics, config_.integerTightening})),
      impl_(std::make_unique<Impl>())
{
    impl_->synchronize(native_);
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

ELINAPolyhedraDomain::ELINAPolyhedraDomain(
    const ELINAPolyhedraDomain& other)
    : NumericalDomain(other), config_(other.config_), native_(other.native_),
      impl_(std::make_unique<Impl>())
{
    impl_->synchronize(native_);
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
    native_ = other.native_;
    impl_ = std::make_unique<Impl>();
    impl_->synchronize(native_);
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
        config_.diagnostics->report({operation, approximation, std::move(reason)});
}

void ELINAPolyhedraDomain::synchronizeELINA(
    OperationKind operation, const std::string& fallbackOperation)
{
    impl_->synchronize(native_);
    if (!fallbackOperation.empty())
    {
        report(operation, ApproximationKind::UnsupportedFallback,
               elinaUnsupportedFallbackReason(fallbackOperation), false);
        return;
    }
    if (!impl_->usable)
    {
        report(operation, ApproximationKind::UnsupportedFallback,
               "ELINA Polyhedra synchronization failed; native B0 remains authoritative",
               false);
        return;
    }
    report(operation,
           impl_->approximated ? ApproximationKind::SoundOverApproximation
                               : native_.lastOperation().approximation,
           impl_->approximated
               ? "real strict constraint was conservatively closed for ELINA"
               : native_.lastOperation().reason,
           !impl_->approximated && native_.lastOperation().best);
}

#define SVF_ELINA_FORWARD_MUTATION(method, operation, ...) \
    do                                                        \
    {                                                         \
        native_.method(__VA_ARGS__);                          \
        synchronizeELINA(operation);                          \
    } while (false)

void ELINAPolyhedraDomain::assign(Variable target,
                                  const LinearExpression& expression)
{
    SVF_ELINA_FORWARD_MUTATION(assign, OperationKind::Assignment, target,
                               expression);
}

void ELINAPolyhedraDomain::assign(Variable target,
                                  const TreeExpression& expression)
{
    native_.assign(target, expression);
    synchronizeELINA(OperationKind::Assignment,
                     expression.asLinear() ? std::string() : "tree assignment");
}

void ELINAPolyhedraDomain::assignParallel(
    const LinearAssignmentList& assignments)
{
    SVF_ELINA_FORWARD_MUTATION(assignParallel, OperationKind::Assignment,
                               assignments);
}

void ELINAPolyhedraDomain::substitute(
    Variable target, const LinearExpression& expression)
{
    SVF_ELINA_FORWARD_MUTATION(substitute, OperationKind::Substitution, target,
                               expression);
}

void ELINAPolyhedraDomain::substituteParallel(
    const LinearAssignmentList& assignments)
{
    SVF_ELINA_FORWARD_MUTATION(substituteParallel,
                               OperationKind::Substitution, assignments);
}

void ELINAPolyhedraDomain::assume(const LinearConstraint& constraint)
{
    SVF_ELINA_FORWARD_MUTATION(assume, OperationKind::Assumption, constraint);
}

void ELINAPolyhedraDomain::assumeAll(
    const LinearConstraintSet& constraints)
{
    SVF_ELINA_FORWARD_MUTATION(assumeAll, OperationKind::Assumption,
                               constraints);
}

void ELINAPolyhedraDomain::assume(const TreeConstraint& constraint)
{
    native_.assume(constraint);
    synchronizeELINA(OperationKind::Assumption, "tree assumption");
}

void ELINAPolyhedraDomain::forget(Variable variable)
{
    SVF_ELINA_FORWARD_MUTATION(forget, OperationKind::Forget, variable);
}

void ELINAPolyhedraDomain::project(const std::vector<Variable>& retained)
{
    SVF_ELINA_FORWARD_MUTATION(project, OperationKind::Forget, retained);
}

void ELINAPolyhedraDomain::expand(Variable source,
                                  const std::vector<Variable>& copies)
{
    native_.expand(source, copies);
    synchronizeELINA(OperationKind::Expand, "expand");
}

void ELINAPolyhedraDomain::fold(Variable target,
                                const std::vector<Variable>& folded)
{
    native_.fold(target, folded);
    synchronizeELINA(OperationKind::Fold, "fold");
}

#undef SVF_ELINA_FORWARD_MUTATION

CheckResult ELINAPolyhedraDomain::entails(
    const LinearConstraint& constraint) const
{
    return native_.entails(constraint);
}

Interval ELINAPolyhedraDomain::bound(Variable variable) const
{
    return native_.bound(variable);
}

Interval ELINAPolyhedraDomain::bound(
    const LinearExpression& expression) const
{
    return native_.bound(expression);
}

std::vector<Variable> ELINAPolyhedraDomain::supportVariables() const
{
    return native_.supportVariables();
}

LinearConstraintSet ELINAPolyhedraDomain::toConstraints() const
{
    return native_.toConstraints();
}

void ELINAPolyhedraDomain::close()
{
    native_.close();
    synchronizeELINA(OperationKind::TopologicalClosure,
                     "topological closure");
}

void ELINAPolyhedraDomain::canonicalize()
{
    native_.canonicalize();
    impl_->synchronize(native_);
    if (impl_->usable && elinaPolyhedraSupports(
                             ELINAOperation::Canonicalization))
    {
        ELINARoundingScope rounding;
        resetResult(impl_->manager);
        elina_abstract0_canonicalize(impl_->manager, impl_->state);
        if (impl_->manager->result.exn == ELINA_EXC_NONE)
        {
            report(OperationKind::Canonicalization,
                   ApproximationKind::Exact, {}, true);
            return;
        }
    }
    report(OperationKind::Canonicalization,
           ApproximationKind::UnsupportedFallback,
           elinaUnsupportedFallbackReason("canonicalization"), false);
}

CheckResult ELINAPolyhedraDomain::backendInclusion(
    const ELINAPolyhedraDomain& other) const
{
    return impl_->inclusion(*other.impl_);
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
    native_.joinWith(requireELINA(other).native_);
    synchronizeELINA(OperationKind::Join);
}

void ELINAPolyhedraDomain::meetDomain(const AbstractDomain& other)
{
    native_.meetWith(requireELINA(other).native_);
    synchronizeELINA(OperationKind::Meet);
}

void ELINAPolyhedraDomain::widenDomain(const AbstractDomain& next)
{
    native_.widenWith(requireELINA(next).native_);
    synchronizeELINA(OperationKind::Widening);
}

void ELINAPolyhedraDomain::narrowDomain(const AbstractDomain& next)
{
    native_.narrowWith(requireELINA(next).native_);
    synchronizeELINA(OperationKind::Narrowing);
}

bool ELINAPolyhedraDomain::isBottomDomain() const
{
    return native_.isBottom();
}

bool ELINAPolyhedraDomain::isTopDomain() const
{
    return native_.isTop();
}

bool ELINAPolyhedraDomain::leqDomain(const AbstractDomain& other) const
{
    return native_.isSubsetOf(requireELINA(other).native_) == CheckResult::True;
}

std::string ELINAPolyhedraDomain::domainToString() const
{
    return "elina-polyhedra(validation-shadow):" + native_.toString();
}

} // namespace SVF::AbstractDomain
