//===- IntervalStateTest.cpp -- Common and sparse state operations -------===//

#include "AE/Core/IntervalState.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>

using namespace SVF;

namespace
{

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

} // namespace

int main()
{
    try
    {
        IntervalState lhs;
        lhs[1] = IntervalValue((s64_t)0, (s64_t)0);
        lhs.store(IntervalState::getVirtualMemAddress(10),
                  IntervalValue((s64_t)0, (s64_t)0));

        IntervalState rhs;
        rhs[1] = IntervalValue((s64_t)2, (s64_t)2);
        rhs.store(IntervalState::getVirtualMemAddress(10),
                  IntervalValue((s64_t)1, (s64_t)3));
        rhs.addToFreedAddrs(IntervalState::getVirtualMemAddress(10));

        IntervalState sparse = lhs;
        for (const auto& [id, value] : rhs.getLocToVal())
            sparse.joinMemoryValue(id, value);
        sparse.joinFreedAddressesFrom(rhs);
        require(sparse[1].getInterval().equals(
                    IntervalValue((s64_t)0, (s64_t)0)),
                "memory-only join changed a ValVar");
        require(sparse.getLocToVal().at(10).getInterval().equals(
                    IntervalValue((s64_t)0, (s64_t)3)),
                "memory-only join lost an ObjVar value");
        require(sparse.isFreedMem(IntervalState::getVirtualMemAddress(10)),
                "freed-address propagation failed");

        IntervalState joined = lhs;
        joined.joinWith(rhs);
        require(joined[1].getInterval().equals(
                    IntervalValue((s64_t)0, (s64_t)2)),
                "whole-state join failed");

        const AbstractDomain::AbstractState& genericLhs = lhs;
        const AbstractDomain::AbstractState& genericRhs = rhs;
        std::unique_ptr<AbstractDomain::AbstractState> genericJoined =
            genericLhs.clone();
        genericJoined->joinWith(genericRhs);
        require(genericJoined->isEquivalentTo(joined) ==
                    AbstractDomain::CheckResult::True,
                "common lattice dispatch failed for IntervalState");

        IntervalState replacement;
        replacement.store(IntervalState::getVirtualMemAddress(20),
                          IntervalValue((s64_t)7, (s64_t)7));
        sparse.replaceMemoryFrom(replacement);
        require(sparse.getLocToVal().count(10) == 0 &&
                    sparse.getLocToVal().count(20) == 1 &&
                    sparse.getVarToVal().count(1) == 1,
                "memory replacement overwrote sparse ValVars");

        std::cout << "SVF IntervalState test: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "SVF IntervalState test: FAIL: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
