//===- SVFIntervalStateRelationalTest.cpp -- Unified state lifecycle -----===//

#include "AE/Core/IntervalState.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace SVF;

namespace
{

SVFRelationalBridge& relation(SVF::IntervalState& state)
{
    SVFRelationalBridge* component = state.getRelationalNumericalState();
    if (!component)
        throw std::runtime_error("missing relational IntervalState component");
    return *component;
}

} // namespace

int main()
{
    try
    {
        const std::vector<TrackedRelationalVariable> variables{
            {11, NumericType::integer(), "x"}};
        SVF::IntervalState current;
        current.initializeRelationalNumericalState(variables);
        relation(current).assignInterval(
            11, IntervalValue((s64_t)0, (s64_t)1));

        SVF::IntervalState copy = current;
        if (copy != current || copy.hash() != current.hash())
            throw std::runtime_error(
                "IntervalState copy/equality/hash omitted the relation");
        relation(copy).assignInterval(
            11, IntervalValue((s64_t)0, (s64_t)2));
        if (copy == current)
            throw std::runtime_error(
                "IntervalState equality ignored a changed relation");

        SVF::IntervalState joined = current;
        joined.joinWith(copy);
        if (!relation(joined).projectInterval(11).equals(
                IntervalValue((s64_t)0, (s64_t)2)) ||
                !(joined >= current) || !(joined >= copy))
            throw std::runtime_error(
                "IntervalState join/order omitted the relation");

        SVF::IntervalState widened = current.widening(copy);
        const IntervalValue expectedWidened(
            BoundedInt(0), IntervalValue::plus_infinity());
        if (!relation(widened).projectInterval(11).equals(expectedWidened) ||
                !(widened >= current) || !(widened >= copy))
            throw std::runtime_error(
                "IntervalState widening omitted the relation");

        SVF::IntervalState narrowed = widened.narrowing(copy);
        if (!relation(narrowed).projectInterval(11).equals(
                IntervalValue((s64_t)0, (s64_t)2)))
            throw std::runtime_error(
                "IntervalState narrowing omitted the relation");

        SVF::IntervalState bottom = current.bottom();
        SVF::IntervalState top = current.top();
        if (!relation(bottom).isBottom() || !relation(top).state().isTop())
            throw std::runtime_error(
                "IntervalState top/bottom omitted the relation");

        bool rejectedAbsentComponent = false;
        try
        {
            SVF::IntervalState absent;
            current.joinWith(absent);
        }
        catch (const std::invalid_argument&)
        {
            rejectedAbsentComponent = true;
        }
        if (!rejectedAbsentComponent)
            throw std::runtime_error(
                "IntervalState join silently dropped an absent relation");

        // Semi-sparse ICFG propagation must not join top-level ValVars.
        IntervalState sparseDestination;
        sparseDestination[21] = IntervalValue((s64_t)0, (s64_t)0);
        sparseDestination.store(
            IntervalState::getVirtualMemAddress(31),
            IntervalValue((s64_t)0, (s64_t)0));
        IntervalState sparseSource;
        sparseSource[21] = IntervalValue((s64_t)10, (s64_t)10);
        sparseSource.store(
            IntervalState::getVirtualMemAddress(31),
            IntervalValue((s64_t)1, (s64_t)2));
        sparseSource.addToFreedAddrs(
            IntervalState::getVirtualMemAddress(31));

        for (const auto& [id, value] : sparseSource.getLocToVal())
            sparseDestination.joinMemoryValue(id, value);
        sparseDestination.joinFreedAddressesFrom(sparseSource);

        if (!sparseDestination[21].getInterval().equals(
                IntervalValue((s64_t)0, (s64_t)0)))
            throw std::runtime_error(
                "semi-sparse memory join incorrectly propagated a ValVar");
        if (!sparseDestination.getLocToVal()
                    .at(31)
                    .getInterval()
                    .equals(IntervalValue((s64_t)0, (s64_t)2)))
            throw std::runtime_error(
                "semi-sparse memory join lost an ObjVar value");
        if (!sparseDestination.isFreedMem(
                IntervalState::getVirtualMemAddress(31)))
            throw std::runtime_error(
                "semi-sparse propagation lost the freed-address set");

        IntervalState replacement;
        replacement.store(IntervalState::getVirtualMemAddress(41),
                          IntervalValue((s64_t)7, (s64_t)7));
        replacement.addToFreedAddrs(
            IntervalState::getVirtualMemAddress(41));
        sparseDestination.replaceMemoryFrom(replacement);
        sparseDestination.replaceFreedAddressesFrom(replacement);
        if (sparseDestination.getLocToVal().count(31) != 0 ||
                sparseDestination.getLocToVal().count(41) != 1 ||
                sparseDestination.getVarToVal().count(21) != 1)
            throw std::runtime_error(
                "component replacement did not preserve sparse ValVars");

        const AbstractState& generic = current;
        if (generic.equals(copy) != CheckResult::False ||
                std::string(generic.name()) != "IntervalState")
            throw std::runtime_error(
                "IntervalState does not implement the common lattice API");

        std::cout << "SVF unified IntervalState test: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SVF unified IntervalState test: FAIL: "
                  << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
