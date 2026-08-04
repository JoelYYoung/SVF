//===- fp-backend-precision-eval.cpp -- Direct Z3/APRON FP comparison ---===//
//
// Compare the two libraries as numerical-operation backends, independently
// of RelationalDomain, joins, or widening.  Z3 proves the exact IEEE result;
// APRON evaluates the same typed tree expression and exposes its interval.
//
//===----------------------------------------------------------------------===//

#include "ap_abstract0.h"
#include "ap_global0.h"
#include "pk.h"
#include "z3++.h"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <utility>
#include <vector>

namespace
{

class Rational
{
public:
    Rational()
    {
        mpq_init(value);
    }

    Rational(const Rational& other)
    {
        mpq_init(value);
        mpq_set(value, other.value);
    }

    Rational& operator=(const Rational& other)
    {
        if (this != &other)
            mpq_set(value, other.value);
        return *this;
    }

    ~Rational()
    {
        mpq_clear(value);
    }

    mpq_ptr get()
    {
        return value;
    }

    mpq_srcptr get() const
    {
        return value;
    }

private:
    mpq_t value;
};

Rational integer(unsigned long value)
{
    Rational result;
    mpq_set_ui(result.get(), value, 1);
    return result;
}

Rational powerOfTwo(int exponent)
{
    Rational result = integer(1);
    if (exponent >= 0)
        mpz_mul_2exp(mpq_numref(result.get()), mpq_numref(result.get()),
                     static_cast<unsigned long>(exponent));
    else
        mpz_mul_2exp(mpq_denref(result.get()), mpq_denref(result.get()),
                     static_cast<unsigned long>(-exponent));
    mpq_canonicalize(result.get());
    return result;
}

Rational add(const Rational& lhs, const Rational& rhs)
{
    Rational result;
    mpq_add(result.get(), lhs.get(), rhs.get());
    return result;
}

std::string rationalString(mpq_srcptr value)
{
    char* raw = mpq_get_str(nullptr, 10, value);
    if (!raw)
        throw std::bad_alloc();
    std::string result(raw);

    void* (*allocate)(std::size_t) = nullptr;
    void* (*reallocate)(void*, std::size_t, std::size_t) = nullptr;
    void (*deallocate)(void*, std::size_t) = nullptr;
    mp_get_memory_functions(&allocate, &reallocate, &deallocate);
    deallocate(raw, result.size() + 1);
    return result;
}

std::string csv(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char character : value)
    {
        if (character == '"')
            escaped.push_back('"');
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

struct Format
{
    const char* name;
    unsigned exponentBits;
    unsigned significandBits;
    ap_texpr_rtype_t apronType;
    bool apronHasType;
};

enum class Rounding
{
    NearestEven,
    Up
};

struct Scenario
{
    const char* name;
    Rational lhs;
    Rational rhs;
    Rational expected;
    Rational ulp;
    Rounding rounding;
};

z3::expr roundingExpr(z3::context& context, Rounding rounding)
{
    Z3_ast value = rounding == Rounding::NearestEven
                       ? Z3_mk_fpa_round_nearest_ties_to_even(context)
                       : Z3_mk_fpa_round_toward_positive(context);
    return z3::to_expr(context, value);
}

z3::expr fpFromRational(z3::context& context, const z3::sort& sort,
                        const z3::expr& rounding, const Rational& value)
{
    const std::string literal = rationalString(value.get());
    z3::expr real = context.real_val(literal.c_str());
    return z3::to_expr(
        context,
        Z3_mk_fpa_to_fp_real(context, rounding, real, sort));
}

bool z3Proves(const Format& format, const Scenario& scenario)
{
    z3::context context;
    z3::sort sort =
        context.fpa_sort(format.exponentBits, format.significandBits);
    z3::expr rounding = roundingExpr(context, scenario.rounding);
    z3::expr lhs = fpFromRational(context, sort, rounding, scenario.lhs);
    z3::expr rhs = fpFromRational(context, sort, rounding, scenario.rhs);
    z3::expr expected =
        fpFromRational(context, sort, rounding, scenario.expected);
    z3::expr sum = z3::to_expr(
        context, Z3_mk_fpa_add(context, rounding, lhs, rhs));

    z3::solver solver(context);
    solver.add(sum != expected);
    return solver.check() == z3::unsat;
}

ap_texpr0_t* apronConstant(const Rational& value)
{
    return ap_texpr0_cst_scalar_mpq(const_cast<mpq_ptr>(value.get()));
}

struct ApronResult
{
    bool containsExpected = false;
    bool singletonExpected = false;
    bool transferExact = false;
    std::string widthUlps;
    std::string interval;
};

ApronResult apronEvaluate(const Format& format, const Scenario& scenario)
{
    ap_manager_t* manager = pk_manager_alloc(true);
    if (!manager)
        throw std::runtime_error("failed to allocate APRON NewPolka manager");

    ap_abstract0_t* state = ap_abstract0_top(manager, 0, 1);
    const ap_texpr_rdir_t direction =
        scenario.rounding == Rounding::NearestEven ? AP_RDIR_NEAREST
                                                    : AP_RDIR_UP;
    ap_texpr0_t* expression = ap_texpr0_binop(
        AP_TEXPR_ADD, apronConstant(scenario.lhs),
        apronConstant(scenario.rhs), format.apronType, direction);
    ap_abstract0_t* assigned =
        ap_abstract0_assign_texpr(manager, false, state, 0, expression, nullptr);
    ap_texpr0_free(expression);
    ap_abstract0_free(manager, state);
    if (!assigned)
    {
        ap_manager_free(manager);
        throw std::runtime_error("APRON assignment returned null");
    }

    ApronResult result;
    result.transferExact = ap_manager_get_flag_exact(manager);

    ap_interval_t* bound = ap_abstract0_bound_dimension(manager, assigned, 0);
    if (!bound)
    {
        ap_abstract0_free(manager, assigned);
        ap_manager_free(manager);
        throw std::runtime_error("APRON bound query returned null");
    }

    if (ap_scalar_infty(bound->inf) != 0 ||
            ap_scalar_infty(bound->sup) != 0)
    {
        result.interval = "unbounded";
        result.widthUlps = "infinity";
    }
    else
    {
        Rational lower;
        Rational upper;
        ap_mpq_set_scalar(lower.get(), bound->inf, MPFR_RNDD);
        ap_mpq_set_scalar(upper.get(), bound->sup, MPFR_RNDU);
        result.containsExpected =
            mpq_cmp(lower.get(), scenario.expected.get()) <= 0 &&
            mpq_cmp(scenario.expected.get(), upper.get()) <= 0;
        result.singletonExpected =
            mpq_cmp(lower.get(), upper.get()) == 0 &&
            mpq_cmp(lower.get(), scenario.expected.get()) == 0;

        Rational width;
        Rational widthInUlps;
        mpq_sub(width.get(), upper.get(), lower.get());
        mpq_div(widthInUlps.get(), width.get(), scenario.ulp.get());
        if (mpq_sgn(widthInUlps.get()) == 0)
            result.widthUlps = "0";
        else
        {
            std::ostringstream widthText;
            widthText << std::scientific << std::setprecision(6)
                      << mpq_get_d(widthInUlps.get());
            result.widthUlps = widthText.str();
        }

        const std::string lowerText = rationalString(lower.get());
        const std::string upperText = rationalString(upper.get());
        if (lowerText.size() + upperText.size() <= 160)
            result.interval = "[" + lowerText + "," + upperText + "]";
        else
        {
            std::ostringstream intervalText;
            intervalText << '[' << std::scientific << std::setprecision(6)
                         << mpq_get_d(lower.get()) << ','
                         << mpq_get_d(upper.get()) << "] (large rationals)";
            result.interval = intervalText.str();
        }
    }

    ap_interval_free(bound);
    ap_abstract0_free(manager, assigned);
    ap_manager_free(manager);
    return result;
}

unsigned digits10(unsigned significandBits)
{
    return static_cast<unsigned>(std::floor(
        static_cast<double>(significandBits - 1) * std::log10(2.0)));
}

unsigned maxDigits10(unsigned significandBits)
{
    return static_cast<unsigned>(std::ceil(
        1.0 + static_cast<double>(significandBits) * std::log10(2.0)));
}

std::vector<Scenario> scenarios(const Format& format)
{
    const int precision = static_cast<int>(format.significandBits);
    const Rational one = integer(1);
    const Rational halfUlp = powerOfTwo(-precision);
    const Rational ulp = powerOfTwo(1 - precision);
    const Rational next = add(one, ulp);
    const Rational large = powerOfTwo(precision);

    std::vector<Scenario> result;
    result.push_back(
        {"half-ulp-nearest-even", one, halfUlp, one, ulp,
         Rounding::NearestEven});
    result.push_back(
        {"half-ulp-toward-positive", one, halfUlp, next, ulp,
         Rounding::Up});
    result.push_back(
        {"one-ulp-nearest-even", one, ulp, next, ulp,
         Rounding::NearestEven});
    result.push_back(
        {"integer-precision-boundary", large, one, large, integer(2),
         Rounding::NearestEven});
    return result;
}

} // namespace

int main()
{
    const std::vector<Format> formats = {
        {"binary16", 5, 11, AP_RTYPE_REAL, false},
        {"binary32", 8, 24, AP_RTYPE_SINGLE, true},
        {"binary64", 11, 53, AP_RTYPE_DOUBLE, true},
        {"binary80", 15, 64, AP_RTYPE_EXTENDED, true},
        {"binary128", 15, 113, AP_RTYPE_QUAD, true},
    };

    std::cout
        << "format,significand_bits,digits10,max_digits10,scenario,"
           "z3_proved,apron_status,apron_contains_exact,"
           "apron_singleton_exact,apron_transfer_exact,apron_width_ulps,"
           "apron_interval\n";
    for (const Format& format : formats)
    {
        for (const Scenario& scenario : scenarios(format))
        {
            try
            {
                const bool proved = z3Proves(format, scenario);
                if (!format.apronHasType)
                {
                    std::cout << csv(format.name) << ','
                              << format.significandBits << ','
                              << digits10(format.significandBits) << ','
                              << maxDigits10(format.significandBits) << ','
                              << csv(scenario.name) << ','
                              << (proved ? "true" : "false")
                              << ",unsupported,false,false,false,n/a,"
                              << csv("APRON has no binary16 rounding type")
                              << '\n';
                    continue;
                }
                const ApronResult apron = apronEvaluate(format, scenario);
                std::cout << csv(format.name) << ','
                          << format.significandBits << ','
                          << digits10(format.significandBits) << ','
                          << maxDigits10(format.significandBits) << ','
                          << csv(scenario.name) << ','
                          << (proved ? "true" : "false") << ','
                          << (apron.containsExpected ? "encloses" : "mismatch")
                          << ','
                          << (apron.containsExpected ? "true" : "false")
                          << ','
                          << (apron.singletonExpected ? "true" : "false")
                          << ',' << (apron.transferExact ? "true" : "false")
                          << ',' << csv(apron.widthUlps) << ','
                          << csv(apron.interval) << '\n';
            }
            catch (const std::exception& error)
            {
                std::cout << csv(format.name) << ','
                          << format.significandBits << ','
                          << digits10(format.significandBits) << ','
                          << maxDigits10(format.significandBits) << ','
                          << csv(scenario.name)
                          << ",error,error,error,error,error,error,"
                          << csv(error.what()) << '\n';
            }
        }
    }
    return 0;
}
