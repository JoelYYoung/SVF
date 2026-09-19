//===- box-storage-observer.cpp -- Box storage census -------------------//
//
//                     SVF: Static Value-Flow Analysis
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
//===----------------------------------------------------------------------===//

#include "AE/Svfexe/AbstractInterpretation.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#ifdef SVF_BOX_STORAGE_TELEMETRY
#include <array>
#include <unordered_map>
#include <unordered_set>
#endif
#include <vector>

using namespace SVF;
using namespace SVFUtil;

#ifdef SVF_BOX_STORAGE_TELEMETRY
namespace
{
using namespace SVF::AbstractDomain;

struct StorageEvents
{
    std::array<std::uint64_t,
        static_cast<std::size_t>(BoxStorageEventKind::Count)>
        counts{};
    std::unordered_set<std::uint64_t> livePages;
    std::size_t peakLivePages = 0;
} storageEvents;

std::size_t eventIndex(BoxStorageEventKind kind)
{
    return static_cast<std::size_t>(kind);
}

void collectStorageEvent(const BoxStorageEvent &event)
{
    ++storageEvents.counts[eventIndex(event.kind)];
    if (event.kind == BoxStorageEventKind::PageAllocate ||
            event.kind == BoxStorageEventKind::PageDetach ||
            event.kind == BoxStorageEventKind::JoinMaterializedPage)
    {
        storageEvents.livePages.insert(event.pageId);
        storageEvents.peakLivePages =
            std::max(storageEvents.peakLivePages, storageEvents.livePages.size());
    }
    else if (event.kind == BoxStorageEventKind::PageRelease)
        storageEvents.livePages.erase(event.pageId);
}

struct CarrierStorage
{
    std::size_t states = 0;
    std::size_t bottomStates = 0;
    std::size_t logicalPageReferences = 0;
    std::size_t directoryAllocatedBytes = 0;
    std::size_t pageObjectBytes = 0;
    std::size_t slotsPerPage = 0;
    std::unordered_map<std::uint64_t, BoxStoragePageSnapshot> pages;

    void add(const BoxDomain &box)
    {
        const BoxStorageSnapshot snapshot = box.storageSnapshot();
        ++states;
        bottomStates += snapshot.bottom;
        logicalPageReferences += snapshot.pages.size();
        directoryAllocatedBytes += snapshot.directoryAllocatedBytes;
        if (slotsPerPage == 0)
            slotsPerPage = snapshot.slotsPerPage;
        else if (slotsPerPage != snapshot.slotsPerPage)
            throw std::runtime_error("inconsistent Box page width");
        if (!snapshot.pages.empty())
            pageObjectBytes = snapshot.pageShallowBytes / snapshot.pages.size();
        for (const BoxStoragePageSnapshot &page : snapshot.pages)
            pages.emplace(page.pageId, page);
    }

    void print(const char *role) const
    {
        std::unordered_set<std::string> contents;
        std::size_t occupied = 0;
        std::size_t rationalBytes = 0;
        std::size_t maxReferences = 0;
        for (const auto &entry : pages)
        {
            const BoxStoragePageSnapshot &page = entry.second;
            contents.insert(page.canonicalContent);
            occupied += page.occupiedSlots;
            rationalBytes += page.rationalUsedLimbBytes;
            maxReferences = std::max(maxReferences, page.referenceCount);
        }
        const std::size_t uniquePages = pages.size();
        SVFUtil::outs()
                << "BOX_STORAGE_CARRIER role=" << role << " states=" << states
                << " bottom_states=" << bottomStates
                << " logical_page_refs=" << logicalPageReferences
                << " unique_pages=" << uniquePages
                << " content_classes=" << contents.size() << " cow_saved_refs="
                << (logicalPageReferences >= uniquePages
                    ? logicalPageReferences - uniquePages
                    : 0)
                << " duplicate_physical_pages="
                << (uniquePages >= contents.size() ? uniquePages - contents.size() : 0)
                << " occupied_slots=" << occupied
                << " empty_slots=" << uniquePages * slotsPerPage - occupied
                << " max_reference_count=" << maxReferences
                << " directory_allocated_bytes=" << directoryAllocatedBytes
                << " unique_page_shallow_bytes=" << uniquePages * pageObjectBytes
                << " unique_index_shallow_bytes=" << occupied * sizeof(Variable)
                << " unique_interval_shallow_bytes=" << occupied * sizeof(Interval)
                << " unique_rational_used_limb_bytes=" << rationalBytes << '\n';
    }
};

const BoxAddressDomain *
boxAddress(const SVF::AbstractDomain::AbstractDomain &domain)
{
    return dynamic_cast<const BoxAddressDomain *>(&domain);
}
} // namespace
#endif

int main(int argc, char **argv)
{
#ifdef SVF_BOX_STORAGE_TELEMETRY
    BoxDomain::setStorageEventSink(collectStorageEvent);
#endif
    std::vector<char *> arguments(argv, argv + argc);
    arguments.reserve(static_cast<std::size_t>(argc) + 3);
    const auto hasOption = [&](std::string_view option)
    {
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument(argv[index]);
            if (argument == option ||
                    (argument.size() > option.size() &&
                     argument.compare(0, option.size(), option) == 0 &&
                     argument[option.size()] == '='))
                return true;
        }
        return false;
    };
    const auto addDefault = [&](std::string_view option, char *value)
    {
        if (!hasOption(option))
            arguments.push_back(value);
    };
    addDefault("-model-consts", const_cast<char *>("-model-consts=true"));
    addDefault("-model-arrays", const_cast<char *>("-model-arrays=true"));
    addDefault("-pre-field-sensitive",
               const_cast<char *>("-pre-field-sensitive=false"));

    const std::vector<std::string> modules = OptionBase::parseOptions(
            static_cast<int>(arguments.size()), arguments.data(),
            "Static Symbolic Execution", "[options] <input-bitcode...>");

    LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(modules);
    SVFIRBuilder builder;
    SVFIR *pag = builder.build();
    AndersenWaveDiff *ander = AndersenWaveDiff::createAndersenWaveDiff(pag);
    builder.updateCallGraph(ander->getCallGraph());

    AbstractInterpretation &ae = AbstractInterpretation::getAEInstance();
    if (Options::BufferOverflowCheck())
        ae.addDetector(std::make_unique<BufOverflowDetector>());
    if (Options::NullDerefCheck())
        ae.addDetector(std::make_unique<NullptrDerefDetector>());
    ae.runOnModule();

#ifdef SVF_BOX_STORAGE_TELEMETRY
    CarrierStorage scalarStorage;
    CarrierStorage nodeStorage;
    const SVF::AbstractDomain::AbstractDomain *scalar =
        ae.getScalarAbstractState();
    if (scalar)
    {
        if (const BoxAddressDomain *product = boxAddress(*scalar))
            scalarStorage.add(product->numerical());
    }
    for (const ICFGNode *node : ae.getAnalyzedNodes())
    {
        if (!ae.hasAbsState(node))
            continue;
        if (const BoxAddressDomain *product = boxAddress(ae.getAbstractState(node)))
            nodeStorage.add(product->numerical());
    }
    scalarStorage.print("scalar");
    nodeStorage.print(scalar ? "memory" : "combined");
    std::unordered_set<std::uint64_t> retainedPages;
    for (const auto &page : scalarStorage.pages)
        retainedPages.insert(page.first);
    for (const auto &page : nodeStorage.pages)
        retainedPages.insert(page.first);
    SVFUtil::outs()
            << "BOX_STORAGE_EVENTS allocate="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageAllocate)]
            << " detach="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageDetach)]
            << " release="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageRelease)]
            << " write_unique="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageWriteUnique)]
            << " erase_unique="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::PageEraseUnique)]
            << " join_shared="
            << storageEvents.counts[eventIndex(BoxStorageEventKind::JoinSharedPage)]
            << " join_materialized="
            << storageEvents
            .counts[eventIndex(BoxStorageEventKind::JoinMaterializedPage)]
            << " live_pages=" << storageEvents.livePages.size()
            << " peak_live_pages=" << storageEvents.peakLivePages
            << " retained_snapshot_pages=" << retainedPages.size() << '\n';
#endif

    for (auto it = pag->getICFG()->begin(); it != pag->getICFG()->end(); ++it)
    {
        const auto *node = it->second;
        if (!ae.hasAbsState(node))
            continue;
        for (const auto *stmt : node->getSVFStmts())
        {
            const SVFVar *value = nullptr;
            const char *operation = nullptr;
            std::string source;
            if (const auto *load = SVFUtil::dyn_cast<LoadStmt>(stmt))
            {
                value = load->getLHSVar();
                source = load->getRHSVar()->getValueName();
                operation = "LOAD";
            }
            else if (const auto *binary = SVFUtil::dyn_cast<BinaryOPStmt>(stmt))
            {
                value = binary->getRes();
                operation = "BINARY";
            }
            if (!value)
                continue;
            SVFUtil::outs() << "AUDIT " << operation << " node=" << node->getId()
                            << " source=" << source
                            << " pointer=" << value->isPointer();
            if (value->isPointer())
            {
#if AUDIT_BOX
                const auto addresses = ae.getAddressSet(value, node);
                SVFUtil::outs() << " empty=" << addresses.isBottom()
                                << " top=" << addresses.isTop()
                                << " addresses=" << addresses.toString();
#else
                const auto addresses = ae.getAbsValue(value, node).getAddrs();
                SVFUtil::outs() << " empty=" << addresses.isBottom()
                                << " count=" << addresses.size();
#endif
            }
            else
            {
#if AUDIT_BOX
                const auto interval = ae.getInterval(value, node);
#else
                const auto interval = ae.getAbsValue(value, node).getInterval();
#endif
                SVFUtil::outs() << " top=" << interval.isTop()
                                << " bottom=" << interval.isBottom()
                                << " interval=" << interval.toString();
            }
            SVFUtil::outs() << '\n';
        }
    }

#if AUDIT_BOX
    if (std::getenv("AUDIT_MEMORY_POLICY"))
    {
        bool checked = false;
        for (auto it = pag->getICFG()->begin();
                it != pag->getICFG()->end() && !checked; ++it)
        {
            const auto *node = it->second;
            if (!ae.hasAbsState(node))
                continue;
            for (const auto *stmt : node->getSVFStmts())
            {
                const auto *load = SVFUtil::dyn_cast<LoadStmt>(stmt);
                if (!load)
                    continue;
                for (NodeID id : ander->getPts(load->getRHSVar()->getId()))
                {
                    const auto *object = SVFUtil::dyn_cast<ObjVar>(pag->getSVFVar(id));
                    if (!object)
                        continue;
                    const auto savedInterval = ae.getInterval(object, node);
                    const auto savedAddresses = ae.getAddressSet(object, node);
                    ae.updateValue(object, SVF::AbstractDomain::Interval::top(),
                                   SVF::AbstractDomain::AddressSet::top(), node);
                    if (!ae.getAddressSet(object, node).isTop())
                        throw std::runtime_error(
                            "Explicit memory Top did not survive round trip");
                    ae.updateValue(object, SVF::AbstractDomain::Interval::top(),
                                   SVF::AbstractDomain::AddressSet::bottom(), node);
                    if (!ae.getAddressSet(object, node).isBottom())
                        throw std::runtime_error(
                            "Explicit memory Empty did not survive round trip");
                    ae.updateValue(object, savedInterval, savedAddresses, node);
                    checked = true;
                    break;
                }
                if (checked)
                    break;
            }
        }
        if (!checked)
            throw std::runtime_error("Memory round-trip test found no target");
        SVFUtil::outs() << "AUDIT_POLICY_PASS Top and Empty memory round trips\n";
    }
#endif

    AndersenWaveDiff::releaseAndersenWaveDiff();
    LLVMModuleSet::releaseLLVMModuleSet();
#ifdef SVF_BOX_STORAGE_TELEMETRY
    BoxDomain::setStorageEventSink(nullptr);
#endif
    return 0;
}

