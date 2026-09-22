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

#include "AE/Core/BoxAddressDomain.h"
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
#ifdef SVF_BOX_STORAGE_TELEMETRY
    BoxAddressDomain product(BoxDomain::top(), MemoryLayout(), true);
    BoxAddressDomain copied = product;
    check(copied.operationVersion() == product.operationVersion(),
          "Product copy lost operation identity");
    copied.setInterval(Variable(9), Interval::singleton(Rational(4)));
    check(copied.operationVersion() != product.operationVersion(),
          "Product mutation retained operation identity");
    const std::uint64_t beforeJoin = product.operationVersion();
    product.joinWith(product);
    check(product.operationVersion() != beforeJoin,
          "Product lattice operation retained operation identity");
#endif
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
struct OperationRecord
{
    AbstractOperationKind operation;
    AbstractOperationPhase phase;
    DomainKind leftKind;
    std::uint64_t elapsedNanoseconds;
    CheckResult result;
};

std::vector<OperationRecord> operationRecords;

void collectOperation(const AbstractOperationEvent& event)
{
    operationRecords.push_back({event.operation, event.phase,
                                event.left->kind(),
                                event.elapsedNanoseconds, event.result});
}

struct MutationRecord
{
    BoxMutationKind kind;
    BoxMutationPhase phase;
    std::uint64_t epoch;
    std::uint64_t stateId;
    std::uint64_t relatedStateId;
    std::vector<Variable> changed;
    std::vector<std::uint8_t> constrainedAfter;
    std::vector<Variable> touched;
};

std::vector<MutationRecord> mutationRecords;
std::vector<BoxStorageEvent> contextualStorageEvents;
std::vector<BoxStorageWorkEvent> contextualWorkEvents;
std::vector<BoxStateEvent> stateEvents;

void collectMutation(const BoxMutationEvent& event)
{
    MutationRecord record{event.kind, event.phase, event.epoch, event.stateId,
                          event.relatedStateId, {}, {}, {}};
    if (event.changedVariables)
    {
        record.changed.assign(event.changedVariables,
                              event.changedVariables +
                              event.changedVariableCount);
        record.constrainedAfter.assign(
            event.changedVariablesConstrainedAfter,
            event.changedVariablesConstrainedAfter +
            event.changedVariableCount);
    }
    if (event.touchedVariables)
        record.touched.assign(event.touchedVariables,
                              event.touchedVariables +
                              event.touchedVariableCount);
    mutationRecords.push_back(std::move(record));
}

void collectContextualStorage(const BoxStorageEvent& event)
{
    contextualStorageEvents.push_back(event);
}

void collectContextualWork(const BoxStorageWorkEvent& event)
{
    contextualWorkEvents.push_back(event);
}

void collectStateEvent(const BoxStateEvent& event)
{
    stateEvents.push_back(event);
}

void mutationEventContract()
{
    const Variable first(1);
    const Variable ninth(9);
    mutationRecords.clear();
    contextualStorageEvents.clear();
    contextualWorkEvents.clear();
    stateEvents.clear();
    BoxDomain::setMutationEventSink(collectMutation);
    BoxDomain::setStorageEventSink(collectContextualStorage);
    BoxDomain::setStorageWorkSink(collectContextualWork);
    BoxDomain::setStateEventSink(collectStateEvent);

    BoxDomain box = BoxDomain::top();
    box.assign(first, LinearExpression(Rational(1)));
    check(mutationRecords.size() == 2 &&
          mutationRecords[0].phase == BoxMutationPhase::Begin &&
          mutationRecords[1].phase == BoxMutationPhase::End &&
          mutationRecords[0].epoch == mutationRecords[1].epoch &&
          mutationRecords[1].changed == std::vector<Variable> {first},
          "mutation assignment epoch");
    check(mutationRecords[1].constrainedAfter ==
          std::vector<std::uint8_t> {1}, "assignment support result");
    check(std::any_of(contextualStorageEvents.begin(),
                      contextualStorageEvents.end(),
                      [](const BoxStorageEvent& event)
    {
        return event.kind == BoxStorageEventKind::PageContentUpdate &&
               event.occupiedSlots == 1 && event.pageShallowBytes > 0 &&
               event.emptySlotShallowBytes > 0;
    }), "post-mutation page footprint missing");

    mutationRecords.clear();
    box.assign(first, LinearExpression(Rational(1)));
    check(mutationRecords.size() == 2 && mutationRecords[1].changed.empty() &&
          mutationRecords[1].touched == std::vector<Variable> {first},
          "mutation no-op was reported as a change");

    mutationRecords.clear();
    box.assignParallel({{first, LinearExpression(Rational(2))},
        {ninth, LinearExpression(Rational(3))}});
    check(mutationRecords.size() == 2 &&
          mutationRecords[0].kind == BoxMutationKind::ParallelAssignment &&
          mutationRecords[1].changed ==
          (std::vector<Variable> {first, ninth}),
          "parallel mutation was split or page-biased");

    const std::size_t eventsBeforeCopy = stateEvents.size();
    BoxDomain copy = box;
    check(stateEvents.size() == eventsBeforeCopy + 1 &&
          stateEvents.back().kind == BoxStateEventKind::CopyConstruct &&
          stateEvents.back().sourceStateId != stateEvents.back().stateId,
          "state copy edge missing");

    mutationRecords.clear();
    contextualStorageEvents.clear();
    contextualWorkEvents.clear();
    copy.forget(first);
    check(mutationRecords.size() == 2 &&
          mutationRecords[1].changed == std::vector<Variable> {first},
          "forget mutation variables");
    check(mutationRecords[1].constrainedAfter ==
          std::vector<std::uint8_t> {0}, "forget support result");
    const std::uint64_t forgetEpoch = mutationRecords[1].epoch;
    check(std::any_of(contextualStorageEvents.begin(),
                      contextualStorageEvents.end(),
                      [forgetEpoch](const BoxStorageEvent& event)
    {
        return event.kind == BoxStorageEventKind::PageDetach &&
               event.mutationEpoch == forgetEpoch && event.stateId != 0;
    }), "page detach lacks mutation context");
    check(std::any_of(contextualWorkEvents.begin(), contextualWorkEvents.end(),
                      [forgetEpoch](const BoxStorageWorkEvent& event)
    {
        return event.kind == BoxStorageWorkKind::Clone &&
               event.mutationEpoch == forgetEpoch && event.stateId != 0;
    }), "slot-copy work lacks mutation context");
    check(std::any_of(contextualStorageEvents.begin(),
                      contextualStorageEvents.end(),
                      [forgetEpoch](const BoxStorageEvent& event)
    {
        return event.kind == BoxStorageEventKind::PageContentUpdate &&
               event.mutationEpoch == forgetEpoch &&
               event.occupiedSlots == 0;
    }), "post-erase page footprint missing");

    BoxDomain bottoming = box;
    mutationRecords.clear();
    contextualStorageEvents.clear();
    contextualWorkEvents.clear();
    bottoming.assume(lessEqual(LinearExpression(first),
                               LinearExpression(Rational(0))));
    check(bottoming.isBottom() && mutationRecords.size() == 2 &&
          mutationRecords[1].kind == BoxMutationKind::Assumption &&
          mutationRecords[1].changed ==
          (std::vector<Variable> {first, ninth}) &&
          mutationRecords[1].touched == std::vector<Variable> {first},
          "bottom transition confused snapshot variables with physical touches");
    const std::uint64_t bottomEpoch = mutationRecords[1].epoch;
    check(std::any_of(contextualStorageEvents.begin(),
                      contextualStorageEvents.end(),
                      [bottomEpoch](const BoxStorageEvent& event)
    {
        return event.kind == BoxStorageEventKind::PageDetach &&
               event.mutationEpoch == bottomEpoch;
    }), "bottom transition lost its pre-clear physical write");

    BoxDomain left = BoxDomain::top();
    left.assign(first, LinearExpression(Rational(2)));
    left.assign(ninth, LinearExpression(Rational(3)));
    BoxDomain right = BoxDomain::top();
    right.assign(first, LinearExpression(Rational(2)));
    mutationRecords.clear();
    left.joinWith(right);
    check(mutationRecords.size() == 2 &&
          mutationRecords[0].kind == BoxMutationKind::Join &&
          mutationRecords[0].relatedStateId != 0 &&
          mutationRecords[1].changed == std::vector<Variable> {ninth},
          "join bypass was not captured exactly");

    BoxDomain::setStateEventSink(nullptr);
    BoxDomain::setStorageWorkSink(nullptr);
    BoxDomain::setStorageEventSink(nullptr);
    BoxDomain::setMutationEventSink(nullptr);
    std::cout << "mutation_event_contract=pass exact_changes=checked "
                 "cross_page=checked detach_context=checked copy_edge=checked\n";
}

void operationEventContract()
{
    operationRecords.clear();
    AbstractDomain::setOperationEventSink(collectOperation);
    MemoryLayout layout;
    BoxAddressDomain left(BoxDomain::top(), layout, true);
    BoxAddressDomain right(BoxDomain::top(), layout, true);
    left.setInterval(Variable(1), Interval::singleton(Rational(1)));
    right.setInterval(Variable(1), Interval::singleton(Rational(2)));
    BoxAddressDomain joined = left;
    joined.joinWith(right);
    const CheckResult subset = left.isSubsetOf(joined);
    AbstractDomain::setOperationEventSink(nullptr);

    check(subset == CheckResult::True, "operation event subset result");
    check(operationRecords.size() == 4, "nested operation event leaked");
    check(operationRecords[0].operation == AbstractOperationKind::Join &&
          operationRecords[0].phase == AbstractOperationPhase::Begin &&
          operationRecords[1].operation == AbstractOperationKind::Join &&
          operationRecords[1].phase == AbstractOperationPhase::End &&
          operationRecords[2].operation == AbstractOperationKind::Subset &&
          operationRecords[2].phase == AbstractOperationPhase::Begin &&
          operationRecords[3].operation == AbstractOperationKind::Subset &&
          operationRecords[3].phase == AbstractOperationPhase::End,
          "operation event pairing");
    for (const OperationRecord& record : operationRecords)
        check(record.leftKind == DomainKind::Product,
              "operation event non-product");
    check(operationRecords[3].result == CheckResult::True,
          "operation event query result");
    std::cout << "operation_event_contract=pass product_only=checked pairs=2\n";
}

void directoryContract()
{
    BoxDomain seed = BoxDomain::top();
    for (unsigned page = 0; page < 17; ++page)
        seed.assign(Variable(page * 8), LinearExpression(Rational(page)));
    const auto before = seed.storageSnapshot();
    BoxDomain copy = seed;
    check(copy.storageSnapshot().directoryId == before.directoryId,
          "copy did not share directory");
    copy.assign(Variable(8), LinearExpression(Rational(90)));
    const auto after = copy.storageSnapshot();
    check(after.directoryId != before.directoryId && after.pages.size() == 17,
          "write did not detach directory");
    for (unsigned page = 0; page < 17; ++page)
        check((before.pages[page].pageId == after.pages[page].pageId) == (page != 1),
              "directory detach changed unrelated page ownership");
#ifdef SVF_BOX_WHOLE_DIRECTORY
    check(std::string(BoxDomain::storageRepresentation()).find("whole/") == 0 &&
          before.directoryChunks.empty() && after.directoryChunks.empty() &&
          before.directoryAllocatedBytes == before.directoryRootAllocatedBytes,
          "whole control is not a flat directory");
#else
    check(before.directoryChunks.size() == 3 && after.directoryChunks.size() == 3,
          "chunk control has wrong physical shape");
    for (unsigned chunk = 0; chunk < 3; ++chunk)
        check((before.directoryChunks[chunk].chunkId == after.directoryChunks[chunk].chunkId) == (chunk != 0),
              "write detached wrong directory chunks");
#endif
    copy.forget(Variable(8));
    copy.assign(Variable(8), LinearExpression(Rational(1)));
    check(copy.isEquivalentTo(seed) == CheckResult::True &&
          copy.serializeRaw() == seed.serializeRaw(), "directory erase/reinsert changed semantics");
    // A missing slot in an existing page must not change the old snapshot.
    copy.assign(Variable(12), LinearExpression(Rational(12)));
    check(seed.bound(Variable(12)).isTop(), "directory mutation changed old snapshot");
    std::cout << "directory_contract=pass root_cow=checked physical_shape=checked\n";
}

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
#ifdef SVF_BOX_PAGE_INTERNING
void interningContract()
{
    const Variable x(8), y(9);
    const auto put = [](BoxDomain& state, Variable variable, std::int64_t value)
    {
        state.assign(variable, LinearExpression(Rational(value)));
        state.internPendingPages();
    };
    for (bool afterWrite :
            {
                false, true
            })
    {
        // Every key intentionally collides: no hash-only equality is allowed.
        BoxDomain::PagePool pool(afterWrite, 8, true);
        BoxDomain a = BoxDomain::top();
        BoxDomain b = BoxDomain::top();
        put(a, x, 1);
        const auto before = pool.statistics();
        put(b, x, 1);
        check(pool.statistics().hits > before.hits, "independent equal page not reused");
        const BoxDomain old = a;
        put(a, x, 2);
        check(old.bound(x) == Interval::singleton(Rational(1)) &&
              b.bound(x) == old.bound(x), "interning mutated an old snapshot");
        check(a.bound(x) == Interval::singleton(Rational(2)), "collision changed interval");
        BoxDomain differentVariable = BoxDomain::top();
        put(differentVariable, y, 1);
        check(differentVariable.bound(x).isTop(), "collision conflated coordinates");
        BoxDomain real = BoxDomain::top();
        const Variable realX(8, NumericType::real());
        put(real, realX, 1);
        check(real.bound(realX) == Interval::singleton(Rational(1)), "collision conflated types");

        BoxDomain unique = BoxDomain::top();
        put(unique, Variable(80), 4);
        const auto detaches = pool.statistics().frozenDetaches;
        put(unique, Variable(80), 5);
        check(pool.statistics().frozenDetaches > detaches,
              "sole strong owner modified a weak-indexed page in place");
        unique.forget(Variable(80));
        unique.internPendingPages();
        check(unique.isTop(), "forget did not remove interned page");

        BoxDomain joined = a.join(b);
        joined.internPendingPages();
        check(joined.bound(x) == Interval(Bound::finite(Rational(1)),
                                          Bound::finite(Rational(2))), "interned join mismatch");
        check(joined.meet(a).isEquivalentTo(a) == CheckResult::True, "interned meet mismatch");
        BoxDomain widened = b.widen(a);
        widened.internPendingPages();
        check(widened.bound(x).upper().isPlusInfinity(), "interned widening mismatch");
        BoxDomain narrowed = widened.narrow(a);
        narrowed.internPendingPages();
        check(narrowed.bound(x) == joined.bound(x), "interned narrowing mismatch");
        check(a.serializeRaw() == BoxDomain::fromConstraints(a.toConstraints()).serializeRaw(),
              "interning changed serialization");
        const auto outerHits = pool.statistics().hits;
        {
            BoxDomain::PagePool independent(afterWrite, 8, true);
            BoxDomain otherAnalysis = BoxDomain::top();
            put(otherAnalysis, x, 1);
            check(independent.statistics().hits == 0, "analysis scopes shared a pool");
        }
        BoxDomain resumed = BoxDomain::top();
        put(resumed, x, 1);
        check(pool.statistics().hits > outerHits, "nested scope did not restore outer pool");
    }
    {
        BoxDomain::PagePool pool(false, 2);
        std::vector<BoxDomain> retained;
        for (unsigned i = 0; i < 20; ++i)
        {
            retained.push_back(BoxDomain::top());
            put(retained.back(), Variable(8 * i), i);
        }
        check(pool.statistics().entries <= 2 && pool.statistics().peakEntries <= 2,
              "weak index exceeded capacity");
        check(pool.statistics().evictions >= 18, "bounded pool did not evict");
        retained.clear();
        pool.sweepExpired();
        check(pool.statistics().entries == 0, "weak pool retained dead pages");
    }
    {
        BoxDomain::PagePool disabled(true, 0);
        BoxDomain a = BoxDomain::top();
        put(a, x, 7);
        check(disabled.statistics().entries == 0, "zero budget stored an index entry");
        check(a.bound(x) == Interval::singleton(Rational(7)), "zero budget changed semantics");
    }
    // Existing randomized oracle exercises assignment, snapshots and lattice
    // operations with automatic write-time interning and forced collisions.
    {
        BoxDomain::PagePool pool(true, 64, true);
        contract();
    }
    std::cout << "interning_contract=pass policies=2 collision=forced budget=bounded weak_reclaim=pass\n";
}
#endif
}

int main(int argc, char** argv)
{
    std::cout << "representation=" << BoxDomain::storageRepresentation() << '\n';
    if (argc == 2 && std::string(argv[1]) == "--identity") return 0;
    if (argc == 2 && std::string(argv[1]) == "--interning-identity")
    {
        std::cout << "pool_policy=" << BoxDomain::pageInterningPolicy() << '\n';
        return 0;
    }
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
        directoryContract();
        operationEventContract();
        mutationEventContract();
#endif
#ifdef SVF_BOX_ADAPTIVE_PAGES
        adaptiveContract();
#endif
#ifdef SVF_BOX_PAGE_INTERNING
        interningContract();
#endif
    }
}
