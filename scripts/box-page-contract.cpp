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

#include <algorithm>
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

void workload(unsigned occupancy, unsigned rounds, bool churn = false)
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
        if (churn)
        {
            const auto base = variable.id() / 8 * 8;
            for (unsigned offset = 1; offset < occupancy; ++offset)
                live[destination].forget(Variable(base + offset));
            for (unsigned offset = 1; offset < occupancy; ++offset)
                live[destination].assign(Variable(base + offset),
                                         LinearExpression(Rational(offset)));
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

// Each phase has one timed loop: construction and result checks are outside it.
// This isolates mechanisms; it does not estimate their frequency in real AE.
void phaseWorkload(const std::string& phase, unsigned occupancy, unsigned rounds)
{
    check(occupancy >= 1 && occupancy <= 8 && rounds > 0, "invalid phase workload");
    const std::vector<std::string> phases = {"lookup_hit", "lookup_miss", "copy",
                                             "write_unique", "write_cow", "join_changed", "churn_unique"
                                            };
    check(std::find(phases.begin(), phases.end(), phase) != phases.end(),
          "unknown phase");
    constexpr unsigned pageCount = 64;
    const auto makeBox = [occupancy](unsigned shift)
    {
        BoxDomain box = BoxDomain::top();
        for (unsigned page = 0; page < pageCount; ++page)
            for (unsigned offset = 0; offset < occupancy; ++offset)
                box.assign(Variable(page * 8 + offset),
                           LinearExpression(Rational(offset + shift)));
        return box;
    };
    const BoxDomain seed = makeBox(0);
    const BoxDomain right = makeBox(1);
    BoxDomain unique = makeBox(0);
    std::vector<BoxDomain> live(64, seed);
    // Preconstruct operands so expression allocation is not counted as layout cost.
    const LinearExpression low(Rational(101)), high(Rational(102));
    std::uint64_t observed = 0;
    const auto start = std::chrono::steady_clock::now();
    for (unsigned round = 0; round < rounds; ++round)
    {
        const auto page = round * 31 % pageCount;
        const Variable variable(page * 8 + round % occupancy);
        const auto destination = round % live.size();
        if (phase == "lookup_hit")
            observed += seed.bound(variable).isSingleton();
        else if (phase == "lookup_miss")
            // Includes an in-page hole where available, otherwise a missing page.
            observed += seed.bound(Variable(occupancy < 8 ? page * 8 + occupancy :
                                            pageCount * 8 + page)).isTop();
        else if (phase == "copy")
            live[destination] = round % 2 ? seed : right;
        else if (phase == "write_unique")
            unique.assign(variable, (round / pageCount) % 2 ? high : low);
        else if (phase == "write_cow")
        {
            live[destination] = seed;
            live[destination].assign(variable, (round / pageCount) % 2 ? high : low);
        }
        else if (phase == "join_changed")
        {
            live[destination] = seed;
            live[destination].joinWith(right);
        }
        else
        {
            // At k>=6 this crosses both adaptive thresholds on every cycle.
            // k<=5 is a no-conversion control with the same erase/reinsert pattern.
            for (unsigned slot = 1; slot < occupancy; ++slot)
                unique.forget(Variable(page * 8 + slot));
            for (unsigned slot = 1; slot < occupancy; ++slot)
                unique.assign(Variable(page * 8 + slot), low);
        }
    }
    const auto elapsed = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start).count();
    if (phase == "lookup_hit" || phase == "lookup_miss")
        check(observed == rounds, "phase lookup result mismatch");
    check(seed.bound(Variable(0)) == Interval::singleton(Rational(0)),
          "phase mutated seed snapshot");
    std::uint64_t digest = unique.hash() * 131 + observed;
    for (const auto& box : live) digest = digest * 131 + box.hash();
    std::cout << "phase=" << phase << " occupancy=" << occupancy
              << " rounds=" << rounds << " pages=" << pageCount
              << " seconds=" << elapsed << " semantic_digest=" << digest << '\n';
}

#ifdef SVF_BOX_STORAGE_TELEMETRY
std::vector<BoxStorageWorkEvent> slotWork;

void collectSlotWork(const BoxStorageWorkEvent& event)
{
    slotWork.push_back(event);
}

void storageWorkContract()
{
    const auto count = [](BoxStorageWorkKind kind)
    {
        return std::count_if(slotWork.begin(), slotWork.end(),
                             [kind](const auto& event)
        {
            return event.kind == kind;
        });
    };
    for (unsigned used = 1; used <= 8; ++used)
    {
        BoxDomain seed = BoxDomain::top();
        for (unsigned slot = 0; slot < used; ++slot)
            seed.assign(Variable(slot), LinearExpression(Rational(slot)));
        BoxDomain copy = seed;
        slotWork.clear();
        BoxDomain::setStorageWorkSink(collectSlotWork);
        copy.assign(Variable(0), LinearExpression(Rational(100)));
        BoxDomain::setStorageWorkSink(nullptr);
        check(count(BoxStorageWorkKind::Clone) == 1, "work clone count");
        const auto clone = std::find_if(slotWork.begin(), slotWork.end(),
                                        [](const auto& event)
        {
            return event.kind == BoxStorageWorkKind::Clone;
        });
        auto copied = used;
#ifdef SVF_BOX_ADAPTIVE_PAGES
        if (used >= 6) copied = 8; // Direct-vector placeholders are copied too.
#endif
        check(clone->occupiedSlots == used && clone->copiedSlots == copied &&
              clone->allocatedSlotBytes > 0 &&
              clone->overlappingSlotBytes >= 2 * clone->allocatedSlotBytes,
              "work clone slots/bytes");
        check(seed.bound(Variable(0)) == Interval::singleton(Rational(0)),
              "work telemetry changed seed");
    }
    BoxDomain box = BoxDomain::top();
    slotWork.clear();
    BoxDomain::setStorageWorkSink(collectSlotWork);
    for (unsigned slot = 6; slot > 0; --slot)
        box.assign(Variable(slot - 1), LinearExpression(Rational(slot)));
    for (unsigned slot = 2; slot < 6; ++slot)
        box.forget(Variable(slot));
    BoxDomain::setStorageWorkSink(nullptr);
    check(count(BoxStorageWorkKind::Insert) == 6 &&
          count(BoxStorageWorkKind::Update) == 6 &&
          count(BoxStorageWorkKind::Erase) == 4, "work mutation boundary");
#ifdef SVF_BOX_ADAPTIVE_PAGES
    check(count(BoxStorageWorkKind::Promote) == 1 &&
          count(BoxStorageWorkKind::Demote) == 1 &&
          count(BoxStorageWorkKind::Shrink) == 0, "work format transitions");
#else
    check(count(BoxStorageWorkKind::Promote) == 0 &&
          count(BoxStorageWorkKind::Demote) == 0, "work fixed layout transitions");
#endif
#if defined(SVF_BOX_PACKED_PAGES) || defined(SVF_BOX_ADAPTIVE_PAGES)
    check(count(BoxStorageWorkKind::Grow) == 4, "work geometric growth");
#ifdef SVF_BOX_PACKED_PAGES
    check(count(BoxStorageWorkKind::Shrink) == 1, "work packed shrink");
#endif
    std::size_t shifted = 0;
    for (const auto& event : slotWork)
        if (event.kind == BoxStorageWorkKind::Insert)
            shifted += event.relocatedSlots;
    check(shifted == 15, "work insert shifts");
#else
    check(count(BoxStorageWorkKind::Grow) == 0 &&
          count(BoxStorageWorkKind::Shrink) == 0, "work inline allocation");
#endif
    slotWork.clear();
    std::cout << "storage_work_contract=pass densities=8 transitions=checked\n";
}
#endif

#ifdef SVF_BOX_ADAPTIVE_PAGES
void adaptiveContract()
{
    std::vector<Variable> variables;
    for (unsigned slot = 0; slot < 8; ++slot)
        variables.emplace_back(slot);
    const auto fill = [](BoxDomain& box, unsigned count)
    {
        for (unsigned slot = 0; slot < count; ++slot)
            box.assign(Variable(slot), LinearExpression(Rational(slot)));
    };
    const auto checkLayout = [](const BoxDomain& box, bool direct)
    {
#ifdef SVF_BOX_STORAGE_TELEMETRY
        const auto snapshot = box.storageSnapshot();
        check(snapshot.pages.size() == 1 &&
              snapshot.pages.front().directIndexedSlots == direct,
              "adaptive page hysteresis mismatch");
#else
        (void)box;
        (void)direct;
#endif
    };
    BoxDomain packed = BoxDomain::top();
    fill(packed, 3);
    BoxDomain direct = BoxDomain::top();
    fill(direct, 8);
    for (unsigned slot = 3; slot < 8; ++slot)
        direct.forget(Variable(slot));
    checkLayout(packed, false);
    checkLayout(direct, true);
    check(packed.isEquivalentTo(direct) == CheckResult::True &&
          packed.hash() == direct.hash() && packed.serializeRaw() == direct.serializeRaw(),
          "equal contents depend on physical layout");
    Reference expected;
    for (unsigned slot = 0; slot < 3; ++slot)
        expected.values.emplace(Variable(slot), Interval::singleton(Rational(slot)));
    for (unsigned operation = 0; operation < 4; ++operation)
        for (bool reverse :
                {
                    false, true
                })
        {
            BoxDomain result = reverse ? direct : packed;
            Reference reference = expected;
            combine(result, reference, reverse ? packed : direct, expected,
                    operation, variables);
            verify(result, reference, variables);
        }
    const BoxDomain oldPacked = packed;
    const BoxDomain oldDirect = direct;
    // Churn inside the hysteresis band must preserve the current layout.
    for (unsigned round = 0; round < 128; ++round)
    {
        fill(packed, 5);
        fill(direct, 5);
        checkLayout(packed, false);
        checkLayout(direct, true);
        packed.forget(Variable(4));
        direct.forget(Variable(4));
        checkLayout(packed, false);
        checkLayout(direct, true);
        verify(oldPacked, expected, variables);
        verify(oldDirect, expected, variables);
    }
    fill(packed, 6);
    checkLayout(packed, true);
    for (unsigned slot = 2; slot < 8; ++slot)
        packed.forget(Variable(slot));
    checkLayout(packed, false);
    packed.forget(Variable(0));
    packed.forget(Variable(1));
    check(packed.isTop(), "empty adaptive page not removed");
    for (unsigned round = 0; round < 64; ++round)
    {
        fill(packed, 6);
        const BoxDomain snapshot = packed;
        for (unsigned slot = 2; slot < 6; ++slot)
            packed.forget(Variable(slot));
        checkLayout(packed, false);
        checkLayout(snapshot, true);
        check(snapshot.bound(Variable(5)) == Interval::singleton(Rational(5)) &&
              packed.bound(Variable(5)).isTop(), "conversion changed old snapshot");
    }
    std::cout << "adaptive_contract=pass cross_layout_ops=8 hysteresis_rounds=128 conversion_cycles=64\n";
}
#endif
}

int main(int argc, char** argv)
{
    std::cout << "representation=" << BoxDomain::storageRepresentation() << '\n';
    if (argc == 2 && std::string(argv[1]) == "--identity") return 0;
    if (argc == 4 && std::string(argv[1]) == "--bench")
        workload(std::stoul(argv[2]), std::stoul(argv[3]));
    else if (argc == 4 && std::string(argv[1]) == "--churn")
        workload(std::stoul(argv[2]), std::stoul(argv[3]), true);
    else if (argc == 5 && std::string(argv[1]) == "--phase")
        phaseWorkload(argv[2], std::stoul(argv[3]), std::stoul(argv[4]));
    else
    {
        contract();
#ifdef SVF_BOX_STORAGE_TELEMETRY
        storageWorkContract();
#endif
#ifdef SVF_BOX_ADAPTIVE_PAGES
        adaptiveContract();
#endif
    }
}
