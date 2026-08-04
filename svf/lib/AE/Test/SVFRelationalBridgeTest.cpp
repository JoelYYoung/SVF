//===- SVFRelationalBridgeTest.cpp -- Thin integration-boundary tests ----===//

#include "AE/Core/SVFRelationalBridge.h"
#include "Z3SoundnessChecker.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

using namespace relational;
using namespace relational::test;
using namespace SVF;

int main()
{
    try
    {
        const auto manager = makeOctagonManager();
        SVFRelationalBridge base(
            {{11, NumericType::integer(), "svf_11"},
             {22, NumericType::integer(), "svf_22"}},
            manager);
        base.assignConstant(11, Rational(4));
        base.assignAffine(22, {{11, Rational(1)}}, Rational(2));
        if (base.bound(22).lower().value() != Rational(6) ||
                base.bound(22).upper().value() != Rational(6))
            throw std::runtime_error("bridge lost y := x + 2");

        SVFRelationalBridge branch = base;
        branch.assignConstant(11, Rational(5));
        branch.assignAffine(22, {{11, Rational(1)}}, Rational(2));
        SVFRelationalBridge joined = base;
        joined.joinWith(branch);

        Z3SoundnessChecker checker(base.environment());
        const ProofResult proof =
            checker.checkJoin(base.state(), branch.state(), joined.state());
        if (!proof.proved)
            throw std::runtime_error(proof.detail);

        bool rejected = false;
        try
        {
            joined.forget(999);
        }
        catch (const std::invalid_argument&)
        {
            rejected = true;
        }
        if (!rejected)
            throw std::runtime_error("bridge accepted an untracked NodeID");

        std::cout << "svf-relational-bridge tests: PASS\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "svf-relational-bridge tests: FAIL: " << error.what()
                  << '\n';
        return EXIT_FAILURE;
    }
}
