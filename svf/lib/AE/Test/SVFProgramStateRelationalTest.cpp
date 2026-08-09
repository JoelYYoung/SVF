//===- SVFProgramStateRelationalTest.cpp -- Unified state lifecycle ------===//

#include "AE/Core/AbstractState.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace SVF;
using namespace relational;

namespace
{

SVFRelationalBridge& relation(SVF::AbstractState& state)
{
    SVFRelationalBridge* component = state.getRelationalNumericalState();
    if (!component)
        throw std::runtime_error("missing relational program-state component");
    return *component;
}

} // namespace

int main()
{
    try
    {
        const std::vector<TrackedRelationalVariable> variables{
            {11, NumericType::integer(), "x"}};
        SVF::AbstractState current;
        current.initializeRelationalNumericalState(variables);
        relation(current).assignInterval(
            11, IntervalValue((s64_t)0, (s64_t)1));

        SVF::AbstractState copy = current;
        if (copy != current || copy.hash() != current.hash())
            throw std::runtime_error(
                "program-state copy/equality/hash omitted the relation");
        relation(copy).assignInterval(
            11, IntervalValue((s64_t)0, (s64_t)2));
        if (copy == current)
            throw std::runtime_error(
                "program-state equality ignored a changed relation");

        SVF::AbstractState joined = current;
        joined.joinWith(copy);
        if (!relation(joined).projectInterval(11).equals(
                IntervalValue((s64_t)0, (s64_t)2)) ||
                !(joined >= current) || !(joined >= copy))
            throw std::runtime_error(
                "program-state join/order omitted the relation");

        SVF::AbstractState widened = current.widening(copy);
        const IntervalValue expectedWidened(
            BoundedInt(0), IntervalValue::plus_infinity());
        if (!relation(widened).projectInterval(11).equals(expectedWidened) ||
                !(widened >= current) || !(widened >= copy))
            throw std::runtime_error(
                "program-state widening omitted the relation");

        SVF::AbstractState narrowed = widened.narrowing(copy);
        if (!relation(narrowed).projectInterval(11).equals(
                IntervalValue((s64_t)0, (s64_t)2)))
            throw std::runtime_error(
                "program-state narrowing omitted the relation");

        SVF::AbstractState bottom = current.bottom();
        SVF::AbstractState top = current.top();
        if (!relation(bottom).isBottom() || !relation(top).state().isTop())
            throw std::runtime_error(
                "program-state top/bottom omitted the relation");

        bool rejectedAbsentComponent = false;
        try
        {
            SVF::AbstractState absent;
            current.joinWith(absent);
        }
        catch (const std::invalid_argument&)
        {
            rejectedAbsentComponent = true;
        }
        if (!rejectedAbsentComponent)
            throw std::runtime_error(
                "program-state join silently dropped an absent relation");

        std::cout << "SVF unified relational program-state test: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SVF unified relational program-state test: FAIL: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
