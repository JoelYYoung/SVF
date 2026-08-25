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
    const std::string caseContext =
        " at dimension " + std::to_string(dimensions) + " seed " +
        std::to_string(seed);
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
            "native repeated hull differs from NewPolka" + caseContext);

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
            "incremental constraint meet differs from NewPolka" +
                caseContext);

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
            "generator-backed meet differs from NewPolka" + caseContext);

    if (dimensions >= 2)
    {
        const Variable target = environment.variableOf(0);
        LinearExpression image(environment.variableOf(1));
        image *= Rational(2);
        image.setConstant(Rational(1));
        native.assign(target, image);
        apron = apronAssign(manager, apron, environment, target, image);
        require(apronMatches(manager, native, apron),
                "affine assignment differs from NewPolka" + caseContext);

        const Variable forgotten = environment.variableOf(dimensions - 1);
        native.forget(forgotten);
        apron = apronForget(manager, apron, environment, forgotten);
        require(apronMatches(manager, native, apron),
                "projection/forget differs from NewPolka" + caseContext);
    }
}

void compareLinealityAndPersistentDual(ap_manager_t* manager)
{
    const Variable x(1);
    const Variable y(2);
    const Variable z(3);
    const Variable w(4);
    const VariableEnvironment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"},
         {z, NumericType::real(), "z"}, {w, NumericType::real(), "w"}});

    LinearExpression xMinusY(x);
    xMinusY.setCoefficient(y, Rational(-1));
    LinearExpression xPlusZ(x);
    xPlusZ.setCoefficient(z, Rational(1));
    const LinearConstraintSet initial{
        equal(xMinusY, LinearExpression(Rational(2))),
        greaterEqual(LinearExpression(z), LinearExpression(Rational(-3))),
        lessEqual(xPlusZ, LinearExpression(Rational(5)))};
    ConvexPolyhedraState native =
        ConvexPolyhedraState::fromConstraints(environment, initial);
    ApronValue apron = apronFromConstraints(manager, environment, initial);

    // This shape has two independent lineality directions before clipping:
    // one along x=y and one along the unconstrained w dimension. Joining an
    // identical operand exercises a lazy V union followed by one H recovery.
    native = native.join(native);
    apron = ApronValue(
        manager, ap_abstract0_join(manager, false, apron.get(), apron.get()));
    require(apronMatches(manager, native, apron),
            "explicit lineality round-trip differs from NewPolka");

    LinearExpression yMinusZ(y);
    yMinusZ.setCoefficient(z, Rational(-1));
    const LinearConstraintSet clips{
        greaterEqual(LinearExpression(w), LinearExpression(Rational(-2))),
        lessEqual(LinearExpression(w), LinearExpression(Rational(4))),
        greaterEqual(yMinusZ, LinearExpression(Rational(-7)))};
    native.assumeAll(clips);
    apron = apronMeetConstraints(manager, apron, environment, clips);
    require(apronMatches(manager, native, apron),
            "line-pivot incremental clipping differs from NewPolka");

    LinearExpression largeRow;
    largeRow.setCoefficient(x, Rational(104729));
    largeRow.setCoefficient(y, Rational(-13007));
    largeRow.setCoefficient(z, Rational(8191));
    largeRow.setCoefficient(w, Rational(-4099));
    const LinearConstraintSet largeClip{lessEqual(
        largeRow, LinearExpression(Rational("1234567/97")))};
    native.assumeAll(largeClip);
    apron = apronMeetConstraints(manager, apron, environment, largeClip);
    require(apronMatches(manager, native, apron),
            "large GMP coefficient clipping differs from NewPolka");

    LinearExpression wPlusZ(w);
    wPlusZ.setCoefficient(z, Rational(1));
    const LinearConstraintSet equalityClip{
        equal(wPlusZ, LinearExpression(Rational(1)))};
    native.assumeAll(equalityClip);
    apron = apronMeetConstraints(manager, apron, environment, equalityClip);
    require(apronMatches(manager, native, apron),
            "persistent polar cache equality reduction differs from NewPolka");

    // Repeated export must use the same canonical dual, not rebuild a subtly
    // different affine-hull basis on each H/V transition.
    const std::uint64_t hash = native.hash();
    (void)native.toConstraints();
    native.canonicalize();
    require(native.hash() == hash && apronMatches(manager, native, apron),
            "canonical H/V cache cycle changed a lineal polyhedron");
}

void compareNNCFamilies(ap_manager_t* manager)
{
    for (std::size_t dimensions = 2; dimensions <= 6; ++dimensions)
    {
        std::vector<VariableDeclaration> declarations;
        for (std::size_t dimension = 0; dimension < dimensions; ++dimension)
            declarations.push_back(
                {Variable(static_cast<std::uint32_t>(dimension + 1)),
                 NumericType::real(), "n" + std::to_string(dimension)});
        const VariableEnvironment environment(std::move(declarations));

        constexpr std::uint32_t Trials = 4;
        for (std::uint32_t trial = 0; trial < Trials; ++trial)
        {
            const std::string caseContext =
                " at dimension " + std::to_string(dimensions) + " trial " +
                std::to_string(trial);
            std::mt19937 random(
                0x51A1C7U + static_cast<std::uint32_t>(dimensions) +
                trial * 0x9E3779B9U);
            std::uniform_int_distribution<int> slack(0, 3);

            LinearConstraintSet initial;
            LinearExpression sum;
            for (const VariableDeclaration& declaration :
                 environment.variables())
            {
                initial.push_back(greaterThan(
                    LinearExpression(declaration.variable),
                    LinearExpression(Rational(-4 - slack(random)))));
                initial.push_back(lessEqual(
                    LinearExpression(declaration.variable),
                    LinearExpression(Rational(5 + slack(random)))));
                sum.setCoefficient(declaration.variable, Rational(1));
            }
            initial.push_back(lessThan(
                sum, LinearExpression(Rational(dimensions + slack(random)))));

            ConvexPolyhedraState native =
                ConvexPolyhedraState::fromConstraints(environment, initial);
            ApronValue apron =
                apronFromConstraints(manager, environment, initial);

        // Self-join forces a lazy V-only result. An open facet must remain
        // open rather than becoming the closure during materialization.
            native = native.join(native);
            apron = ApronValue(
                manager,
                ap_abstract0_join(manager, false, apron.get(), apron.get()));
            require(apronMatches(manager, native, apron),
                    "NNC lazy self-hull differs from strict NewPolka" +
                        caseContext);
            require(native.bound(environment.variableOf(0)).lower().isStrict(),
                    "NNC V bound lost an open endpoint" + caseContext);

            LinearExpression clip(environment.variableOf(0));
            clip.setCoefficient(environment.variableOf(1), Rational(-1));
            const LinearConstraintSet strictClip{greaterThan(
                clip, LinearExpression(Rational(-3 - slack(random))))};
            native.assumeAll(strictClip);
            apron =
                apronMeetConstraints(manager, apron, environment, strictClip);
            require(apronMatches(manager, native, apron),
                    "incremental NNC clipping differs from strict NewPolka" +
                        caseContext);

            const Variable target = environment.variableOf(0);
            LinearExpression image(environment.variableOf(1));
            image *= Rational(2);
            image.setConstant(Rational(1));
            native.assign(target, image);
            apron = apronAssign(manager, apron, environment, target, image);
            require(apronMatches(manager, native, apron),
                    "NNC affine image differs from strict NewPolka" +
                        caseContext);

            const Variable forgotten = environment.variableOf(dimensions - 1);
            native.forget(forgotten);
            apron = apronForget(manager, apron, environment, forgotten);
            require(apronMatches(manager, native, apron),
                    "NNC projection differs from strict NewPolka" +
                        caseContext);

            const std::uint64_t hash = native.hash();
            const std::string beforeCanonical = native.toString();
            native.canonicalize();
            require(apronMatches(manager, native, apron),
                    "NNC canonicalization changed the polyhedron" +
                        caseContext);
            require(native.hash() == hash,
                    "NNC canonicalization changed the semantic hash" +
                        caseContext + ": before=" + beforeCanonical +
                        " after=" + native.toString());
        }
    }
}

void compareMixedClosedAndNNC(ap_manager_t* manager)
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    const LinearConstraintSet openRows{
        greaterThan(LinearExpression(x), LinearExpression(Rational(0))),
        lessThan(LinearExpression(x), LinearExpression(Rational(1))),
        equal(LinearExpression(y), LinearExpression(Rational(0)))};
    ConvexPolyhedraState open =
        ConvexPolyhedraState::fromConstraints(environment, openRows);
    ApronValue apronOpen =
        apronFromConstraints(manager, environment, openRows);

    const ConvexPolyhedraState endpoint = point(environment, {2, 0});
    const ApronValue apronEndpoint = apronFromState(manager, endpoint);
    ConvexPolyhedraState hull = open.join(endpoint);
    ApronValue apronHull(
        manager, ap_abstract0_join(manager, false, apronOpen.get(),
                                   apronEndpoint.get()));
    require(apronMatches(manager, hull, apronHull),
            "mixed closed/NNC hull differs from strict NewPolka");
    const Interval hullX = hull.bound(x);
    require(hullX.lower().isStrict() && !hullX.upper().isStrict() &&
                hullX.upper().value() == Rational(2),
            "mixed hull did not preserve open and included endpoints");

    const LinearConstraintSet closedRows{
        greaterEqual(LinearExpression(x), LinearExpression(Rational(-1))),
        lessEqual(LinearExpression(x), LinearExpression(Rational(3))),
        greaterEqual(LinearExpression(y), LinearExpression(Rational(-1)))};
    const ConvexPolyhedraState closed =
        ConvexPolyhedraState::fromConstraints(environment, closedRows);
    const ApronValue apronClosed =
        apronFromConstraints(manager, environment, closedRows);
    ConvexPolyhedraState intersection = open.meet(closed);
    ApronValue apronIntersection(
        manager, ap_abstract0_meet(manager, false, apronOpen.get(),
                                   apronClosed.get()));
    require(apronMatches(manager, intersection, apronIntersection),
            "mixed closed/NNC meet differs from strict NewPolka");
    require(intersection.bound(x).lower().isStrict() &&
                intersection.bound(x).upper().isStrict(),
            "mixed meet closed an NNC endpoint");
}

void compareExpandFold(ap_manager_t* manager)
{
    const Variable x(1);
    const Variable y(2);
    const Variable firstCopy(3);
    const Variable secondCopy(4);
    const VariableEnvironment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    LinearExpression difference(x);
    difference.setCoefficient(y, Rational(-1));
    for (bool strict : {false, true})
    {
        const LinearConstraintSet initial{
            strict ? greaterThan(difference,
                                 LinearExpression(Rational(0)))
                   : greaterEqual(difference,
                                  LinearExpression(Rational(0))),
            strict ? lessThan(difference,
                              LinearExpression(Rational(2)))
                   : lessEqual(difference,
                               LinearExpression(Rational(2))),
            greaterEqual(LinearExpression(y),
                         LinearExpression(Rational(-1))),
            lessEqual(LinearExpression(y),
                      LinearExpression(Rational(3)))};
        const std::string context = strict ? "NNC" : "closed";

        ConvexPolyhedraState native =
            ConvexPolyhedraState::fromConstraints(environment, initial);
        ApronValue apron =
            apronFromConstraints(manager, environment, initial);
        native.expand(
            x, {{firstCopy, NumericType::real(), "x0"},
                {secondCopy, NumericType::real(), "x1"}});
        apron = ApronValue(
            manager,
            ap_abstract0_expand(manager, false, apron.get(), 0, 2));
        require(apronMatches(manager, native, apron),
                context + " polyhedra expand differs from NewPolka");

        LinearExpression copiesDifference(firstCopy);
        copiesDifference.setCoefficient(secondCopy, Rational(-1));
        require(native.entails(equal(copiesDifference,
                                     LinearExpression(Rational()))) !=
                    CheckResult::True,
                context +
                    " polyhedra expand incorrectly equated unrelated copies");

        native.fold(x, {firstCopy, secondCopy});
        ap_dim_t dimensions[] = {0, 2, 3};
        apron = ApronValue(
            manager,
            ap_abstract0_fold(manager, false, apron.get(), dimensions, 3));
        require(apronMatches(manager, native, apron),
                context + " polyhedra fold differs from NewPolka");
    }
}

void compareGeneratorExchange(ap_manager_t* manager)
{
    const Variable x(1);
    const Variable y(2);
    const VariableEnvironment environment(
        {{x, NumericType::real(), "x"}, {y, NumericType::real(), "y"}});
    const PolyhedraGeneratorSet generators{
        {PolyhedraGeneratorKind::ClosurePoint,
         {Rational(0), Rational(0)}},
        {PolyhedraGeneratorKind::Point, {Rational(1), Rational(0)}},
        {PolyhedraGeneratorKind::Ray, {Rational(0), Rational(1)}}};
    ConvexPolyhedraState native =
        ConvexPolyhedraState::fromGenerators(environment, generators);
    const LinearConstraintSet expectedConstraints{
        greaterThan(LinearExpression(x), LinearExpression(Rational(0))),
        lessEqual(LinearExpression(x), LinearExpression(Rational(1))),
        greaterEqual(LinearExpression(y), LinearExpression(Rational(0)))};
    const ApronValue expected =
        apronFromConstraints(manager, environment, expectedConstraints);
    require(apronMatches(manager, native, expected),
            "public point/closure-point/ray import differs from NewPolka");

    native = ConvexPolyhedraState::fromGenerators(environment,
                                                   native.toGenerators());
    require(apronMatches(manager, native, expected),
            "public NNC generator export/import differs from NewPolka");

    const ConvexPolyhedraState line =
        ConvexPolyhedraState::fromGenerators(
            environment,
            {{PolyhedraGeneratorKind::Point,
              {Rational(0), Rational(0)}},
             {PolyhedraGeneratorKind::Line,
              {Rational(0), Rational(1)}}});
    const ApronValue expectedLine = apronFromConstraints(
        manager, environment,
        {equal(LinearExpression(x), LinearExpression(Rational(0)))});
    require(apronMatches(manager, line, expectedLine),
            "public line generator import differs from NewPolka");
}

} // namespace

int main()
{
    try
    {
        ApronManager manager;
        constexpr std::uint32_t StressTrials = 8;
        for (std::size_t dimensions = 2; dimensions <= 6; ++dimensions)
        {
            for (std::uint32_t trial = 0; trial < StressTrials; ++trial)
            {
                compareHullFamilies(
                    manager.get(), dimensions,
                    0xC0FFEEU + static_cast<std::uint32_t>(dimensions) +
                        trial * 0x9E3779B9U);
            }
        }
        compareLinealityAndPersistentDual(manager.get());
        compareNNCFamilies(manager.get());
        compareMixedClosedAndNNC(manager.get());
        compareExpandFold(manager.get());
        compareGeneratorExchange(manager.get());
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
