#include "AE/Core/ELINAPolyhedraDomain.h"
#include "AE/Core/ElinaOctagonDomain.h"

extern "C"
{
#include <ap_abstract0.h>
#include <oct.h>
#include <pk.h>
}

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace AD = SVF::AbstractDomain;

namespace
{

class ApronOracle
{
public:
    explicit ApronOracle(bool polyhedra)
        : manager_(polyhedra ? pk_manager_alloc(true) : oct_manager_alloc()),
          variables_{AD::Variable(3), AD::Variable(800), AD::Variable(11)},
          finiteIntegerModel_(polyhedra)
    {
        if (!manager_)
            throw std::runtime_error("APRON manager allocation failed");
    }

    ~ApronOracle()
    {
        ap_manager_free(manager_);
    }

    const std::vector<AD::Variable>& variables() const
    {
        return variables_;
    }

    ap_dim_t index(AD::Variable variable) const
    {
        const auto found =
            std::find(variables_.begin(), variables_.end(), variable);
        if (found == variables_.end())
            throw std::runtime_error("unknown APRON oracle variable");
        return static_cast<ap_dim_t>(found - variables_.begin());
    }

    ap_linexpr0_t* expression(const AD::LinearExpression& value,
                              bool negate = false) const
    {
        auto* result =
            ap_linexpr0_alloc(AP_LINEXPR_DENSE, variables_.size());
        const AD::Rational sign(negate ? -1 : 1);
        for (const auto& [variable, coefficient] : value.terms())
        {
            const AD::Rational rational = coefficient * sign;
            auto* scalar = ap_scalar_alloc_set_mpq(
                const_cast<mpq_ptr>(rational.value().get_mpq_t()));
            ap_linexpr0_set_coeff_scalar(result, index(variable), scalar);
            ap_scalar_free(scalar);
        }
        const AD::Rational constant = value.constant() * sign;
        auto* scalar = ap_scalar_alloc_set_mpq(
            const_cast<mpq_ptr>(constant.value().get_mpq_t()));
        ap_linexpr0_set_cst_scalar(result, scalar);
        ap_scalar_free(scalar);
        return result;
    }

    ap_lincons0_array_t constraints(
        const AD::LinearConstraintSet& values) const
    {
        auto result = ap_lincons0_array_make(values.size());
        for (std::size_t position = 0; position < values.size(); ++position)
        {
            const AD::ConstraintKind kind = values[position].kind();
            const bool negate = kind == AD::ConstraintKind::LessEqual ||
                                kind == AD::ConstraintKind::LessThan;
            const ap_constyp_t type =
                kind == AD::ConstraintKind::Equal
                    ? AP_CONS_EQ
                    : kind == AD::ConstraintKind::NotEqual
                          ? AP_CONS_DISEQ
                          : kind == AD::ConstraintKind::LessThan ||
                                    kind == AD::ConstraintKind::GreaterThan
                                ? AP_CONS_SUP
                                : AP_CONS_SUPEQ;
            AD::LinearExpression normalized = values[position].expression();
            if (!normalized.terms().empty())
            {
                AD::Rational scale = normalized.terms().begin()->second;
                if (scale.sign() < 0)
                    scale = -scale;
                normalized *= AD::Rational(1) / scale;
            }
            result.p[position] = ap_lincons0_make(
                type, expression(normalized, negate), nullptr);
        }
        return result;
    }

    ap_abstract0_t* from(const AD::LinearConstraintSet& values) const
    {
        auto rows = constraints(values);
        auto* result = ap_abstract0_of_lincons_array(
            manager_, variables_.size(), 0, &rows);
        ap_lincons0_array_clear(&rows);
        return result;
    }

    template <typename Domain>
    void compare(const Domain& state, ap_abstract0_t* expected,
                 const std::string& operation)
    {
        auto* actual = state.isBottom()
                           ? ap_abstract0_bottom(manager_, variables_.size(), 0)
                           : from(state.toConstraints());
        bool equal = true;
        if (finiteIntegerModel_)
        {
            // APRON Polka compares rational polyhedra even when the dimensions
            // are declared integral. Fixed ELINA tightens some rational bounds
            // to an integer-equivalent hull. Compare their concrete integer
            // membership on an explicit small model instead of requiring the
            // two rational representations to be identical.
            for (int x = -8; equal && x <= 8; ++x)
                for (int y = -8; equal && y <= 8; ++y)
                    for (int z = -8; equal && z <= 8; ++z)
                    {
                        auto* point = ap_abstract0_top(
                            manager_, variables_.size(), 0);
                        const int values[] = {x, y, z};
                        for (ap_dim_t dimension = 0;
                             dimension < variables_.size(); ++dimension)
                        {
                            auto* value = expression(AD::LinearExpression(
                                AD::Rational(values[dimension])));
                            point = ap_abstract0_assign_linexpr(
                                manager_, true, point, dimension, value,
                                nullptr);
                            ap_linexpr0_free(value);
                        }
                        const bool inActual =
                            ap_abstract0_is_leq(manager_, point, actual);
                        const bool inExpected =
                            ap_abstract0_is_leq(manager_, point, expected);
                        ap_abstract0_free(manager_, point);
                        ++modelChecks_;
                        if (inActual != inExpected)
                        {
                            std::cerr << "integer-model mismatch at (" << x
                                      << ", " << y << ", " << z << ")\n";
                            equal = false;
                        }
                    }
        }
        else
            equal = ap_abstract0_is_eq(manager_, actual, expected);
        if (!equal)
        {
            std::cerr << "adapter: " << state.toString() << '\n';
            std::cerr << "adapter imported into APRON: ";
            ap_abstract0_fprint(stderr, manager_, actual, nullptr);
            std::cerr << "\nAPRON oracle: ";
            ap_abstract0_fprint(stderr, manager_, expected, nullptr);
            std::cerr << '\n';
        }
        ap_abstract0_free(manager_, actual);
        ++checks_;
        if (!equal)
            throw std::runtime_error(operation + " disagreed with APRON");
    }

    ap_manager_t* manager() const
    {
        return manager_;
    }

    unsigned checks() const
    {
        return checks_;
    }

    unsigned long long modelChecks() const
    {
        return modelChecks_;
    }

private:
    ap_manager_t* manager_;
    std::vector<AD::Variable> variables_;
    bool finiteIntegerModel_;
    unsigned checks_ = 0;
    unsigned long long modelChecks_ = 0;
};

template <typename Domain>
void runFiniteDifferential(ApronOracle& oracle)
{
    const AD::Variable x = oracle.variables()[0];
    const AD::Variable y = oracle.variables()[1];
    const AD::Variable z = oracle.variables()[2];
    std::mt19937 random(7181);
    for (unsigned trial = 0; trial < 25; ++trial)
    {
        Domain left = Domain::top();
        Domain right = Domain::top();
        left.assign(x, AD::LinearExpression(
                           AD::Rational(static_cast<int>(random() % 9) - 4)));
        left.assign(y, AD::LinearExpression(
                           AD::Rational(static_cast<int>(random() % 9) - 4)));
        right.assign(x, AD::LinearExpression(
                            AD::Rational(static_cast<int>(random() % 9) - 4)));
        right.assign(y, AD::LinearExpression(
                            AD::Rational(static_cast<int>(random() % 9) - 4)));

        auto* leftOracle = oracle.from(left.toConstraints());
        auto* rightOracle = oracle.from(right.toConstraints());
        left.joinWith(right);
        auto* expected = ap_abstract0_join(
            oracle.manager(), false, leftOracle, rightOracle);
        oracle.compare(left, expected, "join");
        ap_abstract0_free(oracle.manager(), leftOracle);
        ap_abstract0_free(oracle.manager(), rightOracle);

        const AD::LinearExpression image =
            AD::LinearExpression(x) + AD::LinearExpression(AD::Rational(2));
        left.assign(z, image);
        auto* apronExpression = oracle.expression(image);
        expected = ap_abstract0_assign_linexpr(
            oracle.manager(), true, expected, oracle.index(z),
            apronExpression, nullptr);
        ap_linexpr0_free(apronExpression);
        oracle.compare(left, expected, "assignment");

        const AD::LinearConstraintSet guard{AD::lessEqual(
            AD::LinearExpression(y) - AD::LinearExpression(x),
            AD::LinearExpression(AD::Rational(3)))};
        left.assumeAll(guard);
        auto rows = oracle.constraints(guard);
        expected = ap_abstract0_meet_lincons_array(
            oracle.manager(), true, expected, &rows);
        ap_lincons0_array_clear(&rows);
        oracle.compare(left, expected, "assumption");

        left.forget(y);
        ap_dim_t dimension = oracle.index(y);
        expected = ap_abstract0_forget_array(
            oracle.manager(), true, expected, &dimension, 1, false);
        oracle.compare(left, expected, "forget");

        left.assignParallel({{x, AD::LinearExpression(z)},
                             {z, AD::LinearExpression(x)}});
        ap_dim_t targets[] = {oracle.index(x), oracle.index(z)};
        ap_linexpr0_t* images[] = {oracle.expression(AD::LinearExpression(z)),
                                   oracle.expression(AD::LinearExpression(x))};
        expected = ap_abstract0_assign_linexpr_array(
            oracle.manager(), true, expected, targets, images, 2, nullptr);
        ap_linexpr0_free(images[0]);
        ap_linexpr0_free(images[1]);
        oracle.compare(left, expected, "parallel assignment");

        const AD::LinearExpression preimage =
            AD::LinearExpression(z) + AD::LinearExpression(AD::Rational(1));
        left.substitute(x, preimage);
        apronExpression = oracle.expression(preimage);
        expected = ap_abstract0_substitute_linexpr(
            oracle.manager(), true, expected, oracle.index(x),
            apronExpression, nullptr);
        ap_linexpr0_free(apronExpression);
        oracle.compare(left, expected, "substitution");
        ap_abstract0_free(oracle.manager(), expected);
    }
}

} // namespace

int main()
{
    try
    {
        ApronOracle octagon(false);
        runFiniteDifferential<AD::ElinaOctagonDomain>(octagon);
        ApronOracle polyhedra(true);
        runFiniteDifferential<AD::ELINAPolyhedraDomain>(polyhedra);
        std::cout << "ELINAApronDifferentialTest: PASS octagon="
                  << octagon.checks() << " polyhedra=" << polyhedra.checks()
                  << " finite_integer_points=" << polyhedra.modelChecks()
                  << '\n';
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "ELINAApronDifferentialTest: FAIL: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
