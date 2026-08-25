//===- PolyhedraApronTest.cpp -- Native/NewPolka differential tests -------===//

#include "ApronPolyhedraTestSupport.h"

#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using namespace SVF::AbstractDomain;
using namespace SVF::test;

namespace
{

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

LinearConstraint equalsValue(Variable variable, std::int64_t value)
{
    return equal(LinearExpression(variable),
                 LinearExpression(Rational(value)));
}

ConvexPolyhedraState point(const VariableEnvironment& environment,
                           const std::vector<std::int64_t>& coordinates)
{
    LinearConstraintSet constraints;
    for (std::size_t dimension = 0; dimension < coordinates.size(); ++dimension)
        constraints.push_back(equalsValue(environment.variableOf(dimension),
                                          coordinates[dimension]));
    return ConvexPolyhedraState::fromConstraints(environment, constraints);
}

void compareHullFamilies(ap_manager_t* manager, std::size_t dimensions,
                         std::uint32_t seed)
{
    std::vector<VariableDeclaration> declarations;
    for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
        declarations.push_back({Variable(static_cast<std::uint32_t>(dimension + 1)),
                                NumericType::real(),
                                "v" + std::to_string(dimension)});
    const VariableEnvironment environment(std::move(declarations));

    std::mt19937 random(seed);
    std::uniform_int_distribution<int> coordinate(-7, 7);
    ConvexPolyhedraState native = ConvexPolyhedraState::bottom(environment);
    ApronValue apron(manager, ap_abstract0_bottom(manager, 0, dimensions));
    for (std::size_t index = 0; index < dimensions + 3; ++index)
    {
        std::vector<std::int64_t> coordinates(dimensions);
        for (std::int64_t& value : coordinates)
            value = coordinate(random);
        const ConvexPolyhedraState nativePoint = point(environment, coordinates);
        const ApronValue apronPoint = apronFromState(manager, nativePoint);
        native = native.join(nativePoint);
        apron = ApronValue(manager, ap_abstract0_join(
                                       manager, false, apron.get(),
                                       apronPoint.get()));
    }
    require(apronMatches(manager, native, apron),
            "native repeated hull differs from NewPolka at dimension " +
                std::to_string(dimensions));

    LinearExpression sum;
    for (const VariableDeclaration& declaration : environment.variables())
        sum.setCoefficient(declaration.variable, Rational(1));
    const LinearConstraintSet clips{
        greaterEqual(sum, LinearExpression(Rational(-dimensions))),
        lessEqual(sum, LinearExpression(Rational(dimensions + 2))),
        greaterEqual(LinearExpression(environment.variableOf(0)),
                     LinearExpression(Rational(-3)))};
    native.assumeAll(clips);
    apron = apronMeetConstraints(manager, apron, environment, clips);
    require(apronMatches(manager, native, apron),
            "incremental constraint meet differs from NewPolka");

    ConvexPolyhedraState other = point(
        environment, std::vector<std::int64_t>(dimensions, 0));
    other = other.join(point(environment,
                             std::vector<std::int64_t>(dimensions, 2)));
    const ApronValue apronOther = apronFromState(manager, other);
    native = native.meet(other);
    apron = ApronValue(manager, ap_abstract0_meet(
                                    manager, false, apron.get(),
                                    apronOther.get()));
    require(apronMatches(manager, native, apron),
            "generator-backed meet differs from NewPolka");

    if (dimensions >= 2)
    {
        const Variable target = environment.variableOf(0);
        LinearExpression image(environment.variableOf(1));
        image *= Rational(2);
        image.setConstant(Rational(1));
        native.assign(target, image);
        apron = apronAssign(manager, apron, environment, target, image);
        require(apronMatches(manager, native, apron),
                "affine assignment differs from NewPolka");

        const Variable forgotten = environment.variableOf(dimensions - 1);
        native.forget(forgotten);
        apron = apronForget(manager, apron, environment, forgotten);
        require(apronMatches(manager, native, apron),
                "projection/forget differs from NewPolka");
    }
}

} // namespace

int main()
{
    try
    {
        ApronManager manager;
        for (std::size_t dimensions = 2; dimensions <= 6; ++dimensions)
            compareHullFamilies(manager.get(), dimensions,
                                0xC0FFEEU +
                                    static_cast<std::uint32_t>(dimensions));
        std::cout << "native Polyhedra/APRON NewPolka differential tests passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Polyhedra/APRON differential test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
