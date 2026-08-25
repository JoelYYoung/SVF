//===- BoxOctagonApronTest.cpp -- Clean APRON differential tests --------===//

#include "ApronPolyhedraTestSupport.h"
#include "AE/Core/BoxDomain.h"
#include "AE/Core/OctagonDomain.h"

extern "C"
{
#include "box.h"
#include "oct.h"
}

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace SVF::AbstractDomain;
using namespace SVF::test;

namespace
{

class Manager
{
public:
    explicit Manager(ap_manager_t* manager) : manager_(manager)
    {
        if (manager_ == nullptr)
            throw std::runtime_error("cannot allocate an APRON manager");
    }
    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    ~Manager() { ap_manager_free(manager_); }
    ap_manager_t* get() const { return manager_; }

private:
    ap_manager_t* manager_;
};

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

std::string apronString(ap_manager_t* manager, ap_abstract0_t* value)
{
    FILE* stream = std::tmpfile();
    if (stream == nullptr)
        return "<cannot create diagnostic stream>";
    ap_abstract0_fprint(stream, manager, value, nullptr);
    std::fflush(stream);
    std::fseek(stream, 0, SEEK_END);
    const long size = std::ftell(stream);
    std::rewind(stream);
    std::string result(size > 0 ? static_cast<std::size_t>(size) : 0, '\0');
    if (!result.empty())
        (void)std::fread(result.data(), 1, result.size(), stream);
    std::fclose(stream);
    return result;
}

VariableEnvironment environmentOf(std::size_t dimensions)
{
    std::vector<VariableDeclaration> declarations;
    declarations.reserve(dimensions);
    for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
        declarations.push_back(
            {Variable(static_cast<std::uint32_t>(dimension + 1)),
             NumericType::real(), "v" + std::to_string(dimension)});
    return VariableEnvironment(std::move(declarations));
}

Rational randomRational(std::mt19937& random, int low, int high)
{
    const int numerator =
        std::uniform_int_distribution<int>(low, high)(random);
    const int denominator = std::uniform_int_distribution<int>(1, 5)(random);
    return Rational(Integer(numerator), Integer(denominator));
}

LinearConstraintSet boundedOctagon(const VariableEnvironment& environment,
                                   std::mt19937& random)
{
    std::vector<Rational> centers;
    centers.reserve(environment.size());
    LinearConstraintSet result;
    for (const VariableDeclaration& declaration : environment.variables())
    {
        const Rational center = randomRational(random, -8, 8);
        const Rational radius = randomRational(random, 2, 7);
        centers.push_back(center);
        result.push_back(greaterEqual(LinearExpression(declaration.variable),
                                      LinearExpression(center - radius)));
        result.push_back(lessEqual(LinearExpression(declaration.variable),
                                   LinearExpression(center + radius)));
    }
    for (std::size_t first = 0; first < environment.size(); ++first)
    {
        for (std::size_t second = first + 1; second < environment.size();
             ++second)
        {
            LinearExpression difference(environment.variableOf(first));
            difference.setCoefficient(environment.variableOf(second),
                                      Rational(-1));
            result.push_back(lessEqual(
                difference,
                LinearExpression(centers[first] - centers[second] +
                                 Rational(5))));
        }
    }
    return result;
}

LinearExpression generalExpression(const VariableEnvironment& environment,
                                   std::mt19937& random)
{
    LinearExpression result(randomRational(random, -4, 4));
    for (const VariableDeclaration& declaration : environment.variables())
    {
        Rational coefficient = randomRational(random, -7, 7);
        if (coefficient.isZero())
            coefficient = Rational(2);
        result.setCoefficient(declaration.variable, coefficient);
    }
    return result;
}

template <typename State>
ApronValue apronFromNumericalState(ap_manager_t* manager, const State& state)
{
    if (state.isBottom())
        return ApronValue(
            manager,
            ap_abstract0_bottom(manager, 0, state.environment().size()));
    // APRON's octagon importer recognizes normalized +/-1 coefficients,
    // whereas the native DBM exports unary rows as +/-2*x <= c.  Positive
    // scaling is semantically exact, so normalize every exported row at this
    // test boundary before asking APRON to decide inclusion.
    LinearConstraintSet normalized;
    for (const LinearConstraint& constraint : state.toConstraints())
    {
        LinearExpression expression = constraint.expression();
        Rational scale;
        for (const auto& [variable, coefficient] : expression.terms())
        {
            (void)variable;
            const Rational magnitude =
                coefficient.sign() < 0 ? -coefficient : coefficient;
            if (scale < magnitude)
                scale = magnitude;
        }
        if (!scale.isZero() && scale != Rational(1))
            expression *= Rational(1) / scale;
        normalized.emplace_back(std::move(expression), constraint.kind());
    }
    return apronFromConstraints(manager, state.environment(), normalized);
}

template <typename State>
void requireEquivalent(ap_manager_t* manager, const State& state,
                       const ApronValue& expected,
                       const std::string& context)
{
    const ApronValue actual = apronFromNumericalState(manager, state);
    const bool actualInExpected =
        ap_abstract0_is_leq(manager, actual.get(), expected.get());
    const bool expectedInActual =
        ap_abstract0_is_leq(manager, expected.get(), actual.get());
    require(actualInExpected && expectedInActual,
            context + " differs from APRON (native<=apron=" +
                std::to_string(actualInExpected) + ", apron<=native=" +
                std::to_string(expectedInActual) + "): " + state.toString());
}

template <typename State>
void requireSoundAndAtLeastAsPrecise(
    ap_manager_t* exactManager, ap_manager_t* domainManager,
    const State& state, const ApronValue& exact,
    const ApronValue& domainReference, const std::string& context)
{
    const ApronValue actualExact =
        apronFromNumericalState(exactManager, state);
    require(ap_abstract0_is_leq(exactManager, exact.get(), actualExact.get()),
            context + " is not a sound over-approximation");

    const ApronValue actualDomain =
        apronFromNumericalState(domainManager, state);
    require(
        ap_abstract0_is_leq(domainManager, actualDomain.get(),
                            domainReference.get()),
        context + " is less precise than the APRON domain\n  native: " +
            state.toString() + "\n  APRON: " +
            apronString(domainManager, domainReference.get()));
}

template <typename State>
void compareExactLatticeFamily(ap_manager_t* manager,
                               const VariableEnvironment& environment,
                               const LinearConstraintSet& leftConstraints,
                               const LinearConstraintSet& rightConstraints,
                               const std::string& name)
{
    const State left = State::fromConstraints(environment, leftConstraints);
    const State right = State::fromConstraints(environment, rightConstraints);
    const ApronValue apronLeft =
        apronFromConstraints(manager, environment, leftConstraints);
    const ApronValue apronRight =
        apronFromConstraints(manager, environment, rightConstraints);

    requireEquivalent(manager, left, apronLeft, name + " construction");
    requireEquivalent(
        manager, left.join(right),
        ApronValue(manager, ap_abstract0_join(
                                manager, false, apronLeft.get(),
                                apronRight.get())),
        name + " join");
    requireEquivalent(
        manager, left.meet(right),
        ApronValue(manager, ap_abstract0_meet(
                                manager, false, apronLeft.get(),
                                apronRight.get())),
        name + " meet");

    State forgotten = left;
    const Variable variable = environment.variableOf(environment.size() - 1);
    forgotten.forget(variable);
    requireEquivalent(manager, forgotten,
                      apronForget(manager, apronLeft, environment, variable),
                      name + " forget");
}

void compareBoxFamily(ap_manager_t* boxManager,
                      const VariableEnvironment& environment,
                      const LinearConstraintSet& leftConstraints,
                      const LinearConstraintSet& rightConstraints,
                      std::mt19937& random)
{
    compareExactLatticeFamily<BoxState>(boxManager, environment,
                                        leftConstraints, rightConstraints,
                                        "Box");

    BoxState native = BoxState::fromConstraints(environment, leftConstraints);
    ApronValue apron =
        apronFromConstraints(boxManager, environment, leftConstraints);
    const Variable target = environment.variableOf(0);
    const LinearExpression expression = generalExpression(environment, random);
    native.assign(target, expression);
    apron = apronAssign(boxManager, apron, environment, target, expression);
    requireEquivalent(boxManager, native, apron, "Box affine assignment");
}

void compareOctagonFamily(ap_manager_t* exactManager,
                          ap_manager_t* octagonManager,
                          const VariableEnvironment& environment,
                          const LinearConstraintSet& leftConstraints,
                          const LinearConstraintSet& rightConstraints,
                          std::mt19937& random)
{
    compareExactLatticeFamily<OctagonState>(
        octagonManager, environment, leftConstraints, rightConstraints,
        "Octagon");

    OctagonState assumed =
        OctagonState::fromConstraints(environment, leftConstraints);
    ApronValue apronAssumed =
        apronFromConstraints(octagonManager, environment, leftConstraints);
    ApronValue exactAssumed =
        apronFromConstraints(exactManager, environment, leftConstraints);
    LinearConstraintSet generalConstraints;
    generalConstraints.push_back(lessEqual(
        generalExpression(environment, random), LinearExpression(Rational())));
    generalConstraints.push_back(greaterEqual(
        generalExpression(environment, random),
        LinearExpression(randomRational(random, -3, 3))));
    assumed.assumeAll(generalConstraints);
    apronAssumed = apronMeetConstraints(octagonManager, apronAssumed,
                                        environment, generalConstraints);
    exactAssumed = apronMeetConstraints(exactManager, exactAssumed,
                                        environment, generalConstraints);
    std::string assumeContext = "Octagon general assume";
    for (const LinearConstraint& constraint : generalConstraints)
        assumeContext += "\n  " + constraint.toString(&environment);
    requireSoundAndAtLeastAsPrecise(exactManager, octagonManager, assumed,
                                    exactAssumed, apronAssumed,
                                    assumeContext);

    OctagonState assigned =
        OctagonState::fromConstraints(environment, leftConstraints);
    ApronValue apronAssigned =
        apronFromConstraints(octagonManager, environment, leftConstraints);
    ApronValue exactAssigned =
        apronFromConstraints(exactManager, environment, leftConstraints);
    const Variable target = environment.variableOf(0);
    const LinearExpression expression = generalExpression(environment, random);
    assigned.assign(target, expression);
    apronAssigned = apronAssign(octagonManager, apronAssigned, environment,
                                target, expression);
    exactAssigned = apronAssign(exactManager, exactAssigned, environment,
                                target, expression);
    requireSoundAndAtLeastAsPrecise(exactManager, octagonManager, assigned,
                                    exactAssigned, apronAssigned,
                                    "Octagon general affine assignment");
}

} // namespace

int main()
{
    try
    {
        Manager box(box_manager_alloc());
        Manager octagon(oct_manager_alloc());
        Manager exact(pk_manager_alloc(true));
        constexpr std::uint32_t Trials = 8;
        for (std::size_t dimensions = 2; dimensions <= 6; ++dimensions)
        {
            const VariableEnvironment environment = environmentOf(dimensions);
            for (std::uint32_t trial = 0; trial < Trials; ++trial)
            {
                std::mt19937 random(
                    0xB0C7A60U + static_cast<std::uint32_t>(dimensions) +
                    trial * 0x9E3779B9U);
                const LinearConstraintSet left =
                    boundedOctagon(environment, random);
                const LinearConstraintSet right =
                    boundedOctagon(environment, random);
                compareBoxFamily(box.get(), environment, left, right, random);
                compareOctagonFamily(exact.get(), octagon.get(), environment,
                                     left, right, random);
            }
        }
        std::cout << "native Box/Octagon APRON differential tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Box/Octagon APRON differential test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
