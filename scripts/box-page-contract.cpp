//===- box-page-contract.cpp -- Experimental Box page validation -----------//
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

#include "AE/Core/Expression.h"
#include "AE/Core/NumericalDomain.h"

#include <chrono>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <unordered_map>

using namespace SVF::AbstractDomain;

namespace
{
void check(bool valid, const char* message)
{
    if (!valid)
        throw std::runtime_error(message);
}

struct Reference
{
    bool bottom = false;
    std::map<Variable, Interval> values;

    Interval bound(Variable variable) const
    {
        const auto it = values.find(variable);
        return bottom ? Interval::bottom() :
               it == values.end() ? Interval::top() : it->second;
    }
};

void verify(const BoxDomain& box, const Reference& ref,
            const std::vector<Variable>& variables)
{
    check(box.isBottom() == ref.bottom, "carrier Bottom mismatch");
    for (Variable variable : variables)
        if (box.bound(variable) != ref.bound(variable))
            throw std::runtime_error("bound mismatch v" + std::to_string(variable.id()) +
                                     " actual=" + box.bound(variable).toString() +
                                     " expected=" + ref.bound(variable).toString());
    if (!ref.bottom)
    {
        std::vector<Variable> keys;
        for (const auto& entry : ref.values)
            keys.push_back(entry.first);
        check(box.constrainedVariables() == keys, "iteration mismatch");
        for (Variable upper : variables)
        {
            std::vector<Variable> prefix;
            for (Variable key : keys)
                if (key.id() < upper.id())
                    prefix.push_back(key);
            check(box.constrainedVariablesBefore(upper) == prefix,
                  "prefix iteration mismatch");
        }
    }
}

void combine(BoxDomain& box, Reference& ref, const BoxDomain& rhs,
             const Reference& right, unsigned operation,
             const std::vector<Variable>& variables)
{
    if (operation == 0) box.joinWith(rhs);
    if (operation == 1) box.meetWith(rhs);
    if (operation == 2) box.widenWith(rhs);
    if (operation == 3) box.narrowWith(rhs);
    const bool unionLike = operation == 0 || operation == 2;
    if (ref.bottom || right.bottom)
    {
        if (unionLike && ref.bottom) ref = right;
        else if (!unionLike) ref = {true, {}};
        return;
    }
    Reference result;
    for (Variable variable : variables)
    {
        Interval value = ref.bound(variable);
        const Interval next = right.bound(variable);
        if (operation == 0) value.joinWith(next);
        if (operation == 1) value.meetWith(next);
        if (operation == 2) value.widenWith(next);
        if (operation == 3)
            // Box narrowing restores only infinite endpoints. Interval's
            // convenience narrowWith additionally intersects finite bounds.
            value = Interval(value.lower().isMinusInfinity() ? next.lower() : value.lower(),
                             value.upper().isPlusInfinity() ? next.upper() : value.upper());
        if (value.isBottom())
        {
            result = {true, {}};
            break;
        }
        if (!value.isTop()) result.values.emplace(variable, value);
    }
    ref = std::move(result);
}

void contract()
{
    std::vector<Variable> variables;
    for (unsigned id = 0; id < 8; ++id) variables.emplace_back(id);
    variables.emplace_back(63);
    variables.emplace_back(64);
    variables.emplace_back(1000003);
    variables.emplace_back(0xffffffffu);
    std::vector<BoxDomain> masks;
    std::vector<Reference> references;
    for (unsigned mask = 0; mask < 256; ++mask)
    {
        BoxDomain box = BoxDomain::top();
        Reference ref;
        // Reverse insertion exercises rank and movement, not only appends.
        for (unsigned bit = 8; bit-- > 0;)
            if (mask & (1u << bit))
            {
                box.assign(Variable(bit), LinearExpression(Rational(bit)));
                ref.values.emplace(Variable(bit), Interval::singleton(Rational(bit)));
            }
        verify(box, ref, variables);
        auto restored = NumericalDomain::deserializeRaw(box.serializeRaw());
        check(restored->isEquivalentTo(box) == CheckResult::True,
              "serialization round trip mismatch");
        check(restored->hash() == box.hash(), "semantic hash mismatch");
        masks.push_back(box);
        references.push_back(ref);
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            box.forget(Variable(bit));
            ref.values.erase(Variable(bit));
            verify(box, ref, variables);
            verify(masks.back(), references.back(), variables);
        }
        check(box.isTop(), "last erase did not remove page");
    }
    for (unsigned left = 0; left < 256; ++left)
        for (unsigned right = 0; right < 256; ++right)
        {
            BoxDomain joined = masks[left].join(masks[right]);
            BoxDomain met = masks[left].meet(masks[right]);
            check(joined.isEquivalentTo(masks[left & right]) == CheckResult::True,
                  "all-mask join mismatch");
            check(met.isEquivalentTo(masks[left | right]) == CheckResult::True,
                  "all-mask meet mismatch");
        }

    std::mt19937 random(7319);
    BoxDomain box = BoxDomain::top();
    Reference ref;
    std::vector<BoxDomain> history;
    std::vector<Reference> historyRefs;
    for (unsigned step = 0; step < 2000; ++step)
    {
        const unsigned operation = random() % 10;
        const Variable variable = variables[random() % variables.size()];
        if (operation < 4)
        {
            const Rational value(static_cast<int>(random() % 101) - 50);
            box.assign(variable, LinearExpression(value));
            if (!ref.bottom) ref.values.insert_or_assign(variable, Interval::singleton(value));
        }
        else if (operation == 4)
        {
            box.forget(variable);
            ref.values.erase(variable);
        }
        else if (operation < 9 && !history.empty())
        {
            const auto index = random() % history.size();
            if (operation == 8)
            {
                // Narrowing requires an included next state, unlike join.
                BoxDomain next = box;
                Reference nextRef = ref;
                combine(next, nextRef, history[index], historyRefs[index], 1, variables);
                combine(box, ref, next, nextRef, 3, variables);
            }
            else
                combine(box, ref, history[index], historyRefs[index], operation - 5, variables);
        }
        else
        {
            box = BoxDomain::top();
            ref = {};
        }
        try
        {
            verify(box, ref, variables);
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error("step=" + std::to_string(step) +
                                     " op=" + std::to_string(operation) + " " + error.what());
        }
        history.push_back(box);
        historyRefs.push_back(ref);
        if (history.size() > 16)
        {
            history.erase(history.begin());
            historyRefs.erase(historyRefs.begin());
        }
        for (std::size_t i = 0; i < history.size(); ++i)
            verify(history[i], historyRefs[i], variables);
    }
    BoxDomain typed = BoxDomain::top();
    typed.assign(Variable(7), LinearExpression(Rational(2)));
    bool rejected = false;
    try
    {
        (void)typed.bound(Variable(7, NumericType::real()));
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    check(rejected, "typed ID reuse not rejected");
    const Variable real(100, NumericType::real());
    typed.assign(real, LinearExpression(Rational("1/3")));
    check(typed.bound(real) == Interval::singleton(Rational("1/3")), "rational lost");
    std::cout << "contract=pass masks=256 mask_pairs=65536 random_steps=2000\n";
}

void workload(unsigned occupancy, unsigned rounds)
{
    check(occupancy >= 1 && occupancy <= 8 && rounds > 0, "invalid workload");
    constexpr unsigned pageCount = 512;
    BoxDomain seed = BoxDomain::top();
    for (unsigned page = 0; page < pageCount; ++page)
        for (unsigned offset = 0; offset < occupancy; ++offset)
            seed.assign(Variable(page * 8 + offset), LinearExpression(Rational(offset)));
    std::vector<BoxDomain> live(64, seed);
    const auto start = std::chrono::steady_clock::now();
    for (unsigned round = 0; round < rounds; ++round)
    {
        const auto destination = round % live.size();
        live[destination] = live[(round + 17) % live.size()];
        const Variable variable((round * 31 % pageCount) * 8 + round % occupancy);
        live[destination].assign(variable, LinearExpression(Rational(round % 53)));
        if (round % 3 == 0)
        {
            live[destination].forget(variable);
            live[destination].assign(variable, LinearExpression(Rational(round % 53)));
        }
        if (round % 11 == 0)
            live[destination].joinWith(live[(round + 3) % live.size()]);
    }
    const auto elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start).count();
    std::uint64_t digest = 0;
    for (const auto& box : live) digest = digest * 131 + box.hash();
    std::cout << "occupancy=" << occupancy << " rounds=" << rounds
              << " seconds=" << elapsed << " semantic_digest=" << digest;
#ifdef SVF_BOX_STORAGE_TELEMETRY
    std::unordered_map<std::uint64_t, std::size_t> physical;
    for (const auto& box : live)
        for (const auto& page : box.storageSnapshot().pages)
            physical.emplace(page.pageId, page.shallowBytes);
    std::size_t bytes = 0;
    for (const auto& page : physical) bytes += page.second;
    std::cout << " retained_pages=" << physical.size()
              << " retained_page_shallow_bytes=" << bytes;
#endif
    std::cout << '\n';
}
}

int main(int argc, char** argv)
{
    std::cout << "representation=" << BoxDomain::storageRepresentation() << '\n';
    if (argc == 2 && std::string(argv[1]) == "--identity") return 0;
    if (argc == 4 && std::string(argv[1]) == "--bench")
        workload(std::stoul(argv[2]), std::stoul(argv[3]));
    else contract();
}
