//===- box-read-view-contract.cpp -- Internal read borrowing checks --------//
//
//                     SVF: Static Value-Flow Analysis
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//

#include "AE/Core/BoxAddressDomain.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace SVF::AbstractDomain;

namespace
{
std::size_t allocations = 0;
void* allocate(std::size_t bytes)
{
    ++allocations;
    void* result = std::malloc(bytes);
    if (!result)
        std::abort();
    return result;
}
void* reallocate(void* old, std::size_t, std::size_t bytes)
{
    ++allocations;
    void* result = std::realloc(old, bytes);
    if (!result)
        std::abort();
    return result;
}
void release(void* value, std::size_t)
{
    std::free(value);
}
void check(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}
BoxAddressDomain state()
{
    return BoxAddressDomain(BoxDomain::top(), MemoryLayout(), true);
}
void combine(BoxAddressDomain& left, const BoxAddressDomain& right, unsigned operation)
{
    if (operation == 0) left.joinWith(right);
    if (operation == 1) left.meetWith(right);
    if (operation == 2) left.widenWith(right);
    if (operation == 3) left.narrowWith(right);
}
Interval combined(Interval left, const Interval& right, unsigned operation)
{
    if (operation == 0) left.joinWith(right);
    if (operation == 1 || operation == 3) left.meetWith(right);
    if (operation == 2)
    {
        if (!left.isBottom() && !right.isBottom()) left.widenWith(right);
        else left.joinWith(right);
    }
    return left;
}

void valueAndGuardCases()
{
    const Variable x(3, NumericType::real());
    const std::vector<Interval> values =
    {
        Interval::bottom(), Interval::top(), Interval::singleton(Rational(0)),
        Interval::singleton(Rational("1/3")), Interval::closed(Rational(-2), Rational(4)),
        Interval::singleton(Rational("123456789012345678901234567890123456789/17"))
    };
    unsigned cases = 0;
    for (const Interval& a : values)
        for (const Interval& b : values)
            for (unsigned operation = 0; operation != 4; ++operation)
            {
                auto left = state(), right = state();
                left.setInterval(x, a);
                right.setInterval(x, b);
                const auto leftSnapshot = left, rightSnapshot = right;
                if (operation == 3 && right.isSubsetOf(left) != CheckResult::True)
                {
                    bool rejected = false;
                    try
                    {
                        combine(left, right, operation);
                    }
                    catch (const std::invalid_argument&)
                    {
                        rejected = true;
                    }
                    check(rejected, "invalid narrowing was accepted");
                    check(left.interval(x) == a, "invalid narrowing changed receiver");
                    ++cases;
                    continue;
                }
                combine(left, right, operation);
                check(left.interval(x) == combined(a, b, operation), "value/guard combination changed");
                check(leftSnapshot.interval(x) == a && rightSnapshot.interval(x) == b,
                      "combination changed a retained source");
                check(right.interval(x) == b, "combination changed right operand");
                ++cases;
            }
    auto inactive = state(), active = state();
    inactive.numerical().assign(x, LinearExpression(Rational(7)));
    active.setInterval(x, Interval::singleton(Rational(7)));
    active.addUninitializedNumericalAlternative(x);
    check(inactive.interval(x).isBottom(), "latent payload became initialized");
    check(inactive.isSubsetOf(active) == CheckResult::True, "conditional subset changed");
    auto noPayload = state();
    check(noPayload.isSubsetOf(active) == CheckResult::True, "inactive Top subset fallback changed");
    inactive.joinWith(active);
    check(inactive.interval(x) == Interval::singleton(Rational(7)), "mixed guard lost payload");
    check(inactive.numericalMayBeUninitialized(x), "mixed guard lost uninitialized alternative");
    auto bottom = BoxAddressDomain(BoxDomain::bottom(), MemoryLayout(), true);
    check(bottom.interval(x).isBottom(), "product Bottom query changed");
    bottom.joinWith(active);
    check(bottom.interval(x) == active.interval(x), "Bottom join changed");
    std::cout << "value_cases=" << cases << '\n';
}

void aliasAndLifetimeCases()
{
    auto seed = state();
    for (unsigned id :
            {
                0U, 1U, 7U, 8U, 64U
            })
        seed.setInterval(Variable(id), Interval::singleton(Rational(id)));
    for (unsigned operation = 0; operation != 4; ++operation)
    {
        auto copy = seed;
        combine(copy, copy, operation);
        check(copy.isSubsetOf(seed) == CheckResult::True && seed.isSubsetOf(copy) == CheckResult::True,
              "self combination changed a value");
        const Interval owned = copy.interval(Variable(1));
        copy.setInterval(Variable(0), Interval::singleton(Rational(99)));
        copy.assignValueFrom(Variable(7), copy, Variable(1));
        copy.joinValueFrom(Variable(7), copy, Variable(0));
        copy.resetValue(Variable(1));
        check(owned == Interval::singleton(Rational(1)), "public result is not an owning copy");
        check(seed.interval(Variable(0)) == Interval::singleton(Rational(0)), "COW modified old page");
        check(seed.interval(Variable(7)) == Interval::singleton(Rational(7)), "same-page alias changed snapshot");
        check(copy.interval(Variable(7)) == Interval::closed(Rational(1), Rational(99)),
              "same-product assign/join changed values");
    }
    auto inconsistent = state();
    inconsistent.setInterval(Variable(0, NumericType::real()), Interval::singleton(Rational("1/3")));
    bool rejected = false;
    try
    {
        auto copy = seed;
        copy.joinWith(inconsistent);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    check(rejected, "borrow bypassed typed Variable validation");
}

void allocationProbe()
{
    auto left = state(), right = state();
    for (unsigned id = 0; id != 32; ++id)
    {
        left.setInterval(Variable(id), Interval::closed(Rational(id), Rational(id + 4)));
        right.setInterval(Variable(id), Interval::closed(Rational(id + 1), Rational(id + 3)));
    }
    auto warm = left;
    warm.joinWith(right);
    const auto before = allocations;
    for (unsigned i = 0; i != 1000; ++i)
    {
        auto copy = left;
        copy.joinWith(right);
    }
    const auto used = allocations - before;
    check(left.interval(Variable(0)) == Interval::closed(Rational(0), Rational(4)), "probe changed source");
    std::cout << "join_iterations=1000 coordinates=32 gmp_allocations=" << used << '\n';
}
}

int main()
{
    // Install before constructing any GMP value, in this standalone process only.
    mp_set_memory_functions(allocate, reallocate, release);
    valueAndGuardCases();
    aliasAndLifetimeCases();
    allocationProbe();
    std::cout << "read-view contract passed\n";
}
