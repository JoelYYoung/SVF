//===- VersionedSparseAbstractInterpretation.cpp -- D3 relational AE ------===//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
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

#include "AE/Svfexe/VersionedSparseAbstractInterpretation.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "AE/Core/ICFGWTO.h"
#include "AE/Svfexe/AEWTO.h"
#include "Graphs/ICFG.h"
#include "SVFIR/SVFIR.h"
#include "Util/Options.h"

using namespace SVF;
namespace AD = AbstractDomain;

namespace
{

bool isFlowEdge(const ICFGEdge* edge)
{
    return SVFUtil::isa<IntraCFGEdge>(edge) || SVFUtil::isa<CallCFGEdge>(edge) ||
           SVFUtil::isa<RetCFGEdge>(edge);
}

/// ValVars defined by the statements of `node` (their values exist only after
/// the node executes, so they are not inputs of the node's merge).
std::set<AD::Variable> definedAt(const ICFGNode* node,
                                 const SVFIRAdapter& adapter)
{
    std::set<AD::Variable> result;
    const auto add = [&](const SVFVar* variable)
    {
        const auto* value =
            variable ? SVFUtil::dyn_cast<ValVar>(variable) : nullptr;
        if (value && adapter.contains(*value))
            result.insert(adapter.variable(*value));
    };
    for (const SVFStmt* statement : node->getSVFStmts())
    {
        if (SVFUtil::isa<StoreStmt>(statement))
            continue;
        if (const auto* assign = SVFUtil::dyn_cast<AssignStmt>(statement))
            add(assign->getLHSVar());
        else if (const auto* binary =
                     SVFUtil::dyn_cast<BinaryOPStmt>(statement))
            add(binary->getRes());
        else if (const auto* compare = SVFUtil::dyn_cast<CmpStmt>(statement))
            add(compare->getRes());
        else if (const auto* select = SVFUtil::dyn_cast<SelectStmt>(statement))
            add(select->getRes());
        else if (const auto* phi = SVFUtil::dyn_cast<PhiStmt>(statement))
            add(phi->getRes());
        else if (const auto* unary = SVFUtil::dyn_cast<UnaryOPStmt>(statement))
            add(unary->getRes());
    }
    return result;
}

} // namespace

VersionedSparseAbstractInterpretation::VersionedSparseAbstractInterpretation()
{
    collectCycleHeads();
    buildMemoryVersions();
    computeVersionAvailability();
    reportTelemetry();
}

VersionedSparseAbstractInterpretation::
~VersionedSparseAbstractInterpretation() = default;

void VersionedSparseAbstractInterpretation::collectCycleHeads()
{
    std::function<void(const ICFGWTOComp*)> visit =
        [&](const ICFGWTOComp* component)
    {
        if (const auto* cycle = SVFUtil::dyn_cast<ICFGCycleWTO>(component))
        {
            cycleHeads_.insert(cycle->head()->getICFGNode());
            for (const ICFGWTOComp* inner : cycle->getWTOComponents())
                visit(inner);
        }
    };
    for (const auto& [function, wto] : this->preAnalysis->getFuncToWTO())
    {
        (void)function;
        for (const ICFGWTOComp* component : wto->getWTOComponents())
            visit(component);
    }
}

std::vector<const ObjVar*> VersionedSparseAbstractInterpretation::
staticTargets(const SVFVar* pointer, bool& havocAll) const
{
    havocAll = false;
    std::vector<const ObjVar*> targets;
    if (!pointer)
    {
        havocAll = true;
        return targets;
    }
    const PointsTo& pts =
        this->preAnalysis->getPointerAnalysis()->getPts(pointer->getId());
    // An empty points-to set (for example an integer-to-pointer cast) says
    // nothing about which cell is written; treat it as a write to anything.
    if (pts.empty())
    {
        havocAll = true;
        return targets;
    }
    std::set<NodeID> seen;
    const auto add = [&](NodeID id)
    {
        const auto* object =
            SVFUtil::dyn_cast<ObjVar>(this->svfir->getGNode(id));
        if (object && !object->isPointer() && seen.insert(id).second)
            targets.push_back(object);
    };
    for (NodeID id : pts)
    {
        if (SVFIR::isBlkObj(id))
        {
            havocAll = true;
            return {};
        }
        const SVFVar* node = this->svfir->getGNode(id);
        if (const auto* base = SVFUtil::dyn_cast<BaseObjVar>(node))
        {
            add(id);
            for (NodeID field : this->svfir->getAllFieldsObjVars(base))
                add(field);
        }
        else
            add(id);
    }
    return targets;
}

std::vector<const ObjVar*> VersionedSparseAbstractInterpretation::
reachableTargets(const SVFVar* pointer, bool& havocAll) const
{
    havocAll = false;
    std::vector<const ObjVar*> targets;
    if (!pointer)
    {
        havocAll = true;
        return targets;
    }
    std::set<NodeID> visitedPointers;
    std::set<NodeID> visitedObjects;
    std::deque<NodeID> pointers{pointer->getId()};
    while (!pointers.empty())
    {
        const NodeID current = pointers.front();
        pointers.pop_front();
        if (!visitedPointers.insert(current).second)
            continue;
        const PointsTo& pts =
            this->preAnalysis->getPointerAnalysis()->getPts(current);
        for (NodeID id : pts)
        {
            if (SVFIR::isBlkObj(id))
            {
                havocAll = true;
                return {};
            }
            std::vector<NodeID> objects{id};
            if (const auto* base = SVFUtil::dyn_cast<BaseObjVar>(
                                       this->svfir->getGNode(id)))
                for (NodeID field : this->svfir->getAllFieldsObjVars(base))
                    objects.push_back(field);
            for (NodeID objectId : objects)
            {
                if (!visitedObjects.insert(objectId).second)
                    continue;
                const auto* object = SVFUtil::dyn_cast<ObjVar>(
                                         this->svfir->getGNode(objectId));
                if (!object)
                    continue;
                if (!object->isPointer())
                    targets.push_back(object);
                // A stored pointer exposes its own pointees to the callee.
                pointers.push_back(objectId);
            }
        }
    }
    return targets;
}

void VersionedSparseAbstractInterpretation::failClosed(
    const ICFGNode* node, const std::string& reason) const
{
    const std::string message = "D3 fail-closed at node " +
                                std::to_string(node ? node->getId() : 0) +
                                ": " + reason;
    std::cerr << "AE_D3_FAIL_CLOSED " << message << '\n';
    throw std::runtime_error(message);
}

AD::Variable VersionedSparseAbstractInterpretation::versionFor(
    std::map<std::pair<const void*, NodeID>, AD::Variable>& table,
    const void* site, NodeID object)
{
    const auto key = std::make_pair(site, object);
    const auto found = table.find(key);
    if (found != table.end())
        return found->second;
    const auto* objectVar =
        SVFUtil::cast<ObjVar>(this->svfir->getGNode(object));
    const AD::Variable version = this->adapter_.syntheticVariable(
        this->adapter_.contentVariable(*objectVar).type());
    table.emplace(key, version);
    versionVariables_.insert(version);
    return version;
}

VersionedSparseAbstractInterpretation::VersionMapPtr
VersionedSparseAbstractInterpretation::mergeIncoming(const ICFGNode* node)
{
    static const VersionMapPtr empty = std::make_shared<const VersionMap>();
    const ICFGNode* global = this->icfg->getGlobalICFGNode();
    if (node == global)
        return empty;

    std::vector<VersionMapPtr> incoming;
    for (const ICFGEdge* edge : node->getInEdges())
    {
        if (!isFlowEdge(edge))
            continue;
        const auto source = versionOut_.find(edge->getSrcNode());
        if (source != versionOut_.end())
            incoming.push_back(source->second);
    }
    if (incoming.empty())
    {
        // Program-entry functions start from the global initializers.
        const auto* entry = SVFUtil::dyn_cast<FunEntryICFGNode>(node);
        const auto globalOut = versionOut_.find(global);
        if (!entry || globalOut == versionOut_.end() ||
                !entry->getInEdges().empty())
            return nullptr;
        incoming.push_back(globalOut->second);
    }

    VersionMapPtr result = incoming.front();
    const bool allSame = std::all_of(
        incoming.begin(), incoming.end(),
        [&](const VersionMapPtr& map) { return *map == *result; });
    auto& phis = phiVersions_[node];
    phis.clear();
    if (!allSame)
    {
        VersionMap merged;
        for (const auto& [object, version] : *incoming.front())
        {
            bool presentEverywhere = true;
            bool same = true;
            for (const VersionMapPtr& map : incoming)
            {
                const auto other = map->find(object);
                if (other == map->end())
                {
                    presentEverywhere = false;
                    break;
                }
                same &= other->second == version;
            }
            if (!presentEverywhere)
                continue;
            if (same)
                merged.emplace(object, version);
            else if (cycleHeads_.count(node))
                ++cycleHeadDrops_;
            else
            {
                const AD::Variable phi =
                    versionFor(phiVersionIds_, node, object);
                merged.emplace(object, phi);
                phis.emplace_back(object, phi);
            }
        }
        result = std::make_shared<const VersionMap>(std::move(merged));
    }
    if (phis.empty())
        phiVersions_.erase(node);

    // A fresh activation has no defined stack contents.
    if (const auto* entry = SVFUtil::dyn_cast<FunEntryICFGNode>(node))
    {
        VersionMap fresh;
        bool erased = false;
        for (const auto& [object, version] : *result)
        {
            const BaseObjVar* base = this->svfir->getBaseObject(object);
            if (base && base->isStack() &&
                    base->getFunction() == entry->getFun())
            {
                erased = true;
                continue;
            }
            fresh.emplace(object, version);
        }
        if (erased)
            result = std::make_shared<const VersionMap>(std::move(fresh));
    }
    return result;
}

VersionedSparseAbstractInterpretation::VersionMapPtr
VersionedSparseAbstractInterpretation::transferNode(const ICFGNode* node,
                                                    VersionMapPtr in)
{
    std::optional<VersionMap> edit;
    const auto mutableMap = [&]() -> VersionMap&
    {
        if (!edit)
            edit.emplace(*in);
        return *edit;
    };
    const auto current = [&]() -> VersionMapPtr
    {
        return edit ? std::make_shared<const VersionMap>(*edit) : in;
    };

    for (const SVFStmt* statement : node->getSVFStmts())
    {
        if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(statement))
        {
            bool havocAll = false;
            const std::vector<const ObjVar*> targets =
                staticTargets(store->getLHSVar(), havocAll);
            auto& records = storeVersions_[store];
            records.clear();
            if (havocAll)
            {
                havocStores_.insert(store);
                mutableMap().clear();
                continue;
            }
            for (const ObjVar* object : targets)
            {
                VersionMap& map = mutableMap();
                const auto previous = map.find(object->getId());
                StoreVersion record{
                    object->getId(),
                    versionFor(storeVersionIds_, store, object->getId()),
                    previous != map.end(),
                    previous != map.end() ? previous->second : AD::Variable()};
                map[object->getId()] = record.version;
                records.push_back(record);
            }
        }
        else if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(statement))
            loadVersions_[load] = current();
    }

    // External effects are not StoreStmts. Invalidate every object an
    // external callee can reach through a pointer argument.
    if (const auto* call = SVFUtil::dyn_cast<CallICFGNode>(node))
    {
        const FunObjVar* callee = call->getCalledFunction();
        bool hasBody = false;
        for (const ICFGEdge* edge : call->getOutEdges())
            hasBody |= SVFUtil::isa<CallCFGEdge>(edge);
        // Indirect calls may also reach external targets outside the ICFG.
        const bool external = !callee || SVFUtil::isExtCall(callee) || !hasBody;
        if (external)
        {
            for (const ValVar* argument : call->getActualParms())
            {
                if (!argument || !argument->isPointer())
                    continue;
                bool havocAll = false;
                const std::vector<const ObjVar*> targets =
                    reachableTargets(argument, havocAll);
                if (havocAll)
                {
                    mutableMap().clear();
                    ++externalInvalidations_;
                    break;
                }
                for (const ObjVar* object : targets)
                    if (mutableMap().erase(object->getId()))
                        ++externalInvalidations_;
            }
        }
    }
    return current();
}

void VersionedSparseAbstractInterpretation::buildMemoryVersions()
{
    std::deque<const ICFGNode*> work;
    std::set<const ICFGNode*> queued;
    const auto push = [&](const ICFGNode* node)
    {
        if (queued.insert(node).second)
            work.push_back(node);
    };
    push(this->icfg->getGlobalICFGNode());
    FIFOWorkList<const FunObjVar*> roots = this->collectProgEntryFuns();
    while (!roots.empty())
        push(this->icfg->getFunEntryICFGNode(roots.pop()));

    while (!work.empty())
    {
        const ICFGNode* node = work.front();
        work.pop_front();
        queued.erase(node);
        const VersionMapPtr in = mergeIncoming(node);
        if (!in)
            continue;
        const VersionMapPtr out = transferNode(node, in);
        const auto old = versionOut_.find(node);
        if (old != versionOut_.end() && *old->second == *out)
            continue;
        versionOut_[node] = out;
        for (const ICFGEdge* edge : node->getOutEdges())
            if (isFlowEdge(edge))
                push(edge->getDstNode());
    }
}

void VersionedSparseAbstractInterpretation::computeVersionAvailability()
{
    // A version is available where its definition lies on every path, which
    // is exactly where a summary may legally mention it.
    std::map<const ICFGNode*, std::set<AD::Variable>> out;
    std::deque<const ICFGNode*> work;
    std::set<const ICFGNode*> queued;
    const auto push = [&](const ICFGNode* node)
    {
        if (queued.insert(node).second)
            work.push_back(node);
    };
    const ICFGNode* global = this->icfg->getGlobalICFGNode();
    for (const auto& [node, map] : versionOut_)
    {
        (void)map;
        push(node);
    }
    while (!work.empty())
    {
        const ICFGNode* node = work.front();
        work.pop_front();
        queued.erase(node);
        std::optional<std::set<AD::Variable>> incoming;
        if (node != global)
        {
            for (const ICFGEdge* edge : node->getInEdges())
            {
                if (!isFlowEdge(edge))
                    continue;
                const auto source = out.find(edge->getSrcNode());
                if (source == out.end())
                    continue;
                if (!incoming)
                    incoming = source->second;
                else
                {
                    std::set<AD::Variable> both;
                    std::set_intersection(
                        incoming->begin(), incoming->end(),
                        source->second.begin(), source->second.end(),
                        std::inserter(both, both.end()));
                    incoming = std::move(both);
                }
            }
            if (!incoming && SVFUtil::isa<FunEntryICFGNode>(node) &&
                    node->getInEdges().empty() && out.count(global))
                incoming = out.at(global);
        }
        std::set<AD::Variable> available =
            incoming ? std::move(*incoming) : std::set<AD::Variable>();
        for (const SVFStmt* statement : node->getSVFStmts())
            if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(statement))
                for (const StoreVersion& record : storeVersions_[store])
                    available.insert(record.version);
        const auto phis = phiVersions_.find(node);
        if (phis != phiVersions_.end())
            for (const auto& [object, version] : phis->second)
            {
                (void)object;
                available.insert(version);
            }
        const auto old = out.find(node);
        if (old != out.end() && old->second == available)
            continue;
        out[node] = std::move(available);
        for (const ICFGEdge* edge : node->getOutEdges())
            if (isFlowEdge(edge) && versionOut_.count(edge->getDstNode()))
                push(edge->getDstNode());
    }
    for (auto& [node, available] : out)
    {
        scalarAvailability_[node].insert(available.begin(), available.end());
        versionAvailability_[node] = std::move(available);
    }
}

bool VersionedSparseAbstractInterpretation::strongUpdatable(
    NodeID object) const
{
    const BaseObjVar* base = this->svfir->getBaseObject(object);
    return base && !base->isHeap() && !base->isArray() &&
           !base->isBlackHoleObj();
}

VersionedSparseAbstractInterpretation::State
VersionedSparseAbstractInterpretation::materializedAt(
    const std::vector<AD::Variable>& seeds, const ICFGNode* node)
{
    State state = this->topState();
    if (!seeds.empty())
        this->materializeScalarDefinitions(state, seeds, node);
    return state;
}

void VersionedSparseAbstractInterpretation::recordVersionSummary(
    AD::Variable version, State summary, const ICFGNode* node)
{
    const auto available = scalarAvailability_.find(node);
    std::vector<AD::Variable> keep;
    for (AD::Variable variable : summary.numerical().supportVariables())
        if (variable == version ||
                (available != scalarAvailability_.end() &&
                 available->second.count(variable)))
            keep.push_back(variable);
    if (std::find(keep.begin(), keep.end(), version) == keep.end())
        keep.push_back(version);
    summary.numerical().project(keep);

    auto existing = scalarDefinitions_.find(version);
    if (existing == scalarDefinitions_.end())
        existing = scalarDefinitions_.emplace(version, std::move(summary)).first;
    else
        existing->second.joinWith(summary);

    auto& dependencies = scalarDependencies_[version];
    dependencies.clear();
    for (AD::Variable variable :
            existing->second.numerical().supportVariables())
        if (variable != version)
            dependencies.insert(variable);
    if (dependencies.empty())
        scalarDependencies_.erase(version);
    if (std::getenv("SVF_AE_TRACE_D3"))
        std::cerr << "AE_D3_VERSION node=" << node->getId()
                  << " version=" << version.id() << " summary="
                  << existing->second.numerical().toString() << '\n';
}

void VersionedSparseAbstractInterpretation::handleSVFStatement(
    const SVFStmt* stmt)
{
    inStoreStatement_ = SVFUtil::isa<StoreStmt>(stmt);
    Base::handleSVFStatement(stmt);
    inStoreStatement_ = false;
    if (const auto* load = SVFUtil::dyn_cast<LoadStmt>(stmt))
        versionLoad(load);
    else if (const auto* store = SVFUtil::dyn_cast<StoreStmt>(stmt))
        versionStore(store);
}

void VersionedSparseAbstractInterpretation::updateMemoryValue(
    AD::Location location, const AD::Interval& interval,
    const AD::AddressSet& addresses, const ICFGNode* node)
{
    if (!inStoreStatement_ && !location.isNull())
    {
        const ObjVar* object = this->objectAt(location);
        const auto out = versionOut_.find(node);
        if (object && !object->isPointer() && out != versionOut_.end() &&
                out->second->count(object->getId()))
            failClosed(node, "memory write outside a StoreStmt to versioned "
                             "object " + std::to_string(object->getId()));
    }
    Base::updateMemoryValue(location, interval, addresses, node);
}

void VersionedSparseAbstractInterpretation::assignRelationalStore(
    const ValVar*, AD::Variable, const ICFGNode*)
{
}

void VersionedSparseAbstractInterpretation::assignRelationalLoad(
    const ValVar*, AD::Variable, const ICFGNode*)
{
}

void VersionedSparseAbstractInterpretation::versionStore(
    const StoreStmt* store)
{
    const auto records = storeVersions_.find(store);
    if (records == storeVersions_.end() || havocStores_.count(store))
        return;
    const ICFGNode* node = store->getICFGNode();
    const auto* pointer = SVFUtil::dyn_cast<ValVar>(store->getLHSVar());
    const auto* source = SVFUtil::dyn_cast<ValVar>(store->getRHSVar());
    if (!pointer)
        return;

    const AD::AddressSet pointees = this->getAddressSet(pointer, node);
    const bool unknown = pointees.hasUnknownObject();
    std::set<NodeID> written;
    if (!unknown)
        for (AD::Location location : pointees.locations())
            if (const ObjVar* object = this->objectAt(location))
                written.insert(object->getId());

    std::set<NodeID> versioned;
    for (const StoreVersion& record : records->second)
        versioned.insert(record.object);
    // The static version map cannot be repaired after the fact: a runtime
    // target outside it would leave stale versions reachable. Fail closed.
    if (unknown)
        failClosed(node, "store with unknown target");
    for (NodeID object : written)
    {
        const auto* objectVar =
            SVFUtil::dyn_cast<ObjVar>(this->svfir->getGNode(object));
        if (objectVar && !objectVar->isPointer() && !versioned.count(object))
            failClosed(node, "store target " + std::to_string(object) +
                                 " outside static points-to set");
    }

    const bool sourceTracked = source && this->adapter_.contains(*source) &&
                               !source->isPointer();
    const AD::Interval sourceInterval =
        source ? this->getInterval(source, node) : AD::Interval::top();
    const auto refinement = refinementTrace_.find(node);

    for (const StoreVersion& record : records->second)
    {
        const AD::Variable version = record.version;
        std::vector<AD::Variable> seeds;
        if (sourceTracked)
            seeds.push_back(this->adapter_.variable(*source));
        if (record.hasPrevious)
            seeds.push_back(record.previous);
        if (refinement != refinementTrace_.end())
            for (AD::Variable variable :
                    refinement->second.numerical().supportVariables())
                seeds.push_back(variable);
        State base = materializedAt(seeds, node);
        if (refinement != refinementTrace_.end())
            this->applyScalarRefinement(base, refinement->second);

        const auto assignSource = [&](State& state)
        {
            if (sourceTracked)
                state.assignNumeric(
                    version,
                    AD::LinearExpression(this->adapter_.variable(*source)));
            else
            {
                state.numerical().forget(version);
                this->constrainInterval(state, version, sourceInterval);
            }
        };
        const auto assignPrevious = [&](State& state)
        {
            if (record.hasPrevious)
                state.assignNumeric(version,
                                    AD::LinearExpression(record.previous));
            else
                state.numerical().forget(version);
        };

        State summary = base;
        const bool isWritten = unknown || written.count(record.object);
        if (!isWritten)
            assignPrevious(summary);
        else if (!unknown && written.size() == 1 &&
                 strongUpdatable(record.object))
            assignSource(summary);
        else if (unknown || written.size() > 1)
        {
            // Multi-target weak update. The reference Dense transfer writes
            // only unary bounds for such stores, so a relational join here
            // could not be certified by Post replay. Keep the interval hull
            // of the stored and previous contents.
            AD::Interval hull = sourceInterval;
            if (record.hasPrevious)
                hull.joinWith(base.numerical().bound(record.previous));
            else
                hull = AD::Interval::top();
            summary = this->topState();
            this->constrainInterval(summary, version, hull);
        }
        else
        {
            State other = base;
            assignSource(summary);
            assignPrevious(other);
            summary.joinWith(other);
        }
        recordVersionSummary(version, std::move(summary), node);
    }
}

void VersionedSparseAbstractInterpretation::versionLoad(const LoadStmt* load)
{
    const ICFGNode* node = load->getICFGNode();
    const auto* target = SVFUtil::dyn_cast<ValVar>(load->getLHSVar());
    const auto* pointer = SVFUtil::dyn_cast<ValVar>(load->getRHSVar());
    if (!target || !pointer || !this->adapter_.contains(*target) ||
            target->isPointer())
        return;
    const AD::AddressSet pointees = this->getAddressSet(pointer, node);
    if (pointees.hasUnknownObject())
        return;
    const auto snapshot = loadVersions_.find(load);
    const AD::Variable targetVariable = this->adapter_.variable(*target);
    const auto refinement = refinementTrace_.find(node);

    std::optional<State> joined;
    std::vector<AD::Variable> used;
    for (AD::Location location : pointees.locations())
    {
        const ObjVar* object = this->objectAt(location);
        if (!object || object->isPointer())
            continue;
        const auto version =
            snapshot == loadVersions_.end() || !snapshot->second
            ? VersionMap::const_iterator()
            : snapshot->second->find(object->getId());
        if (snapshot == loadVersions_.end() || !snapshot->second ||
                version == snapshot->second->end())
        {
            // No reaching version: the content is unknown here and the base
            // load has already produced its non-relational value.
            ++loadMisses_;
            return;
        }
        std::vector<AD::Variable> seeds{version->second};
        if (refinement != refinementTrace_.end())
            for (AD::Variable variable :
                    refinement->second.numerical().supportVariables())
                seeds.push_back(variable);
        State alternative = materializedAt(seeds, node);
        if (refinement != refinementTrace_.end())
            this->applyScalarRefinement(alternative, refinement->second);
        alternative.assignNumeric(targetVariable,
                                  AD::LinearExpression(version->second));
        if (!joined)
            joined = std::move(alternative);
        else
            joined->joinWith(alternative);
        used.push_back(version->second);
    }
    if (!joined)
        return;
    for (AD::Variable version : used)
        this->recordRelationalDependency(targetVariable, version);
    this->recordRelationalSummary(targetVariable, *joined, node);
}

void VersionedSparseAbstractInterpretation::versionPhis(const ICFGNode* node)
{
    const auto phis = phiVersions_.find(node);
    if (phis == phiVersions_.end())
        return;
    for (const auto& [object, version] : phis->second)
    {
        std::optional<State> joined;
        bool unknownOperand = false;
        for (const ICFGEdge* edge : node->getInEdges())
        {
            if (!isFlowEdge(edge))
                continue;
            const ICFGNode* predecessor = edge->getSrcNode();
            if (!this->hasAbsState(predecessor) ||
                    this->state(predecessor).isBottom())
                continue;
            const auto out = versionOut_.find(predecessor);
            const auto operand = out == versionOut_.end()
                                 ? VersionMap::const_iterator()
                                 : out->second->find(object);
            if (out == versionOut_.end() || operand == out->second->end())
            {
                unknownOperand = true;
                break;
            }
            std::vector<AD::Variable> seeds{operand->second};
            const auto refinement = refinementTrace_.find(predecessor);
            if (refinement != refinementTrace_.end())
                for (AD::Variable variable :
                        refinement->second.numerical().supportVariables())
                    seeds.push_back(variable);
            State alternative = materializedAt(seeds, predecessor);
            if (refinement != refinementTrace_.end())
                this->applyScalarRefinement(alternative, refinement->second);
            alternative.assignNumeric(version,
                                      AD::LinearExpression(operand->second));
            // A phi summary may mention only the phi result and names
            // available at the merge; never a predecessor-local operand.
            const auto available = scalarAvailability_.find(node);
            const std::set<AD::Variable> local = definedAt(node, this->adapter_);
            std::vector<AD::Variable> keep{version};
            for (AD::Variable variable :
                    alternative.numerical().supportVariables())
                if (variable != version && !local.count(variable) &&
                        available != scalarAvailability_.end() &&
                        available->second.count(variable))
                    keep.push_back(variable);
            alternative.numerical().project(keep);
            if (!joined)
                joined = std::move(alternative);
            else
                joined->joinWith(alternative);
        }
        if (unknownOperand || !joined)
        {
            State top = this->topState();
            scalarDefinitions_.insert_or_assign(version, top);
            scalarDependencies_.erase(version);
            continue;
        }
        recordVersionSummary(version, std::move(*joined), node);
    }
}

void VersionedSparseAbstractInterpretation::projectRefinementToAvailable(
    const ICFGNode* node)
{
    const auto refinement = refinementTrace_.find(node);
    const auto available = scalarAvailability_.find(node);
    if (refinement == refinementTrace_.end() ||
            available == scalarAvailability_.end())
        return;
    const std::set<AD::Variable> local = definedAt(node, this->adapter_);
    std::vector<AD::Variable> keep;
    for (AD::Variable variable :
            refinement->second.numerical().supportVariables())
        if (available->second.count(variable) && !local.count(variable))
            keep.push_back(variable);
    refinement->second.numerical().project(keep);
}

std::vector<AD::Variable> VersionedSparseAbstractInterpretation::mergeInputs(
    const ICFGNode* node) const
{
    std::vector<AD::Variable> inputs;
    const auto available = scalarAvailability_.find(node);
    if (available == scalarAvailability_.end())
        return inputs;
    const std::set<AD::Variable> local = definedAt(node, this->adapter_);
    for (AD::Variable variable : available->second)
        if (!local.count(variable))
            inputs.push_back(variable);
    return inputs;
}

bool VersionedSparseAbstractInterpretation::mergeStatesFromPredecessors(
    const ICFGNode* node)
{
    // Project every predecessor's refinement to the names available on entry
    // to this node before the parent merge joins and applies it. A name the
    // node redefines (loop phi, recomputed SSA value) must not receive a fact
    // about its previous execution. Predecessor traces are restored after the
    // merge because phi alternatives still read them in predecessor scope.
    const std::vector<AD::Variable> inputs = mergeInputs(node);
    const std::set<AD::Variable> keepSet(inputs.begin(), inputs.end());
    std::vector<std::pair<const ICFGNode*, State>> saved;
    std::set<const ICFGNode*> seen;
    for (const ICFGEdge* edge : node->getInEdges())
    {
        const ICFGNode* predecessor = edge->getSrcNode();
        if (!isFlowEdge(edge) || !seen.insert(predecessor).second)
            continue;
        const auto refinement = refinementTrace_.find(predecessor);
        if (refinement == refinementTrace_.end())
            continue;
        if (predecessor != node)
            saved.emplace_back(predecessor, refinement->second);
        std::vector<AD::Variable> keep;
        for (AD::Variable variable :
                refinement->second.numerical().supportVariables())
            if (keepSet.count(variable))
                keep.push_back(variable);
        refinement->second.numerical().project(keep);
    }
    const bool merged = Base::mergeStatesFromPredecessors(node);
    for (auto& [predecessor, refinement] : saved)
        refinementTrace_.insert_or_assign(predecessor, std::move(refinement));
    if (!merged)
        return false;
    projectRefinementToAvailable(node);
    versionPhis(node);
    return true;
}

void VersionedSparseAbstractInterpretation::filterPropagatedState(
    State& state) const
{
    Base::filterPropagatedState(state);
    // Numerical memory payloads live in content versions, not in the flow.
    std::vector<AD::Variable> keep;
    bool dropped = false;
    for (AD::Variable variable : state.numerical().supportVariables())
    {
        if (this->adapter_.contentObject(variable))
            dropped = true;
        else
            keep.push_back(variable);
    }
    if (dropped)
        state.numerical().project(keep);
}

VersionedSparseAbstractInterpretation::State
VersionedSparseAbstractInterpretation::reconstructPostState(
    const ICFGNode* node, const std::set<AD::Variable>& availableScalars)
{
    State reconstructed = Base::reconstructPostState(node, availableScalars);
    // Content versions are analysis-internal coordinates. Scalar closures can
    // pull them into the reconstructed state; replay states never contain
    // them, so they must not survive beyond the relations they imply.
    const auto dropVersions = [&](State& state)
    {
        std::vector<AD::Variable> keep;
        bool dropped = false;
        for (AD::Variable variable : state.numerical().supportVariables())
        {
            if (versionVariables_.count(variable))
                dropped = true;
            else
                keep.push_back(variable);
        }
        if (dropped)
            state.numerical().project(keep);
        for (AD::Variable variable : state.initializedVariables())
            if (versionVariables_.count(variable))
                state.resetValue(variable);
    };
    const auto out = versionOut_.find(node);
    if (out == versionOut_.end() || out->second->empty())
    {
        dropVersions(reconstructed);
        return reconstructed;
    }
    std::vector<AD::Variable> versions;
    for (const auto& [object, version] : *out->second)
    {
        (void)object;
        versions.push_back(version);
    }
    State contents = materializedAt(versions, node);
    std::vector<AD::Variable> keep;
    for (const auto& [object, version] : *out->second)
    {
        const auto* objectVar =
            SVFUtil::cast<ObjVar>(this->svfir->getGNode(object));
        const AD::Variable content =
            this->adapter_.contentVariable(*objectVar);
        contents.numerical().assign(content, AD::LinearExpression(version));
        keep.push_back(content);
    }
    for (AD::Variable variable : contents.numerical().supportVariables())
        if (availableScalars.count(variable))
            keep.push_back(variable);
    contents.numerical().project(keep);
    reconstructed.numerical().meetWith(contents.numerical());
    dropVersions(reconstructed);
    return reconstructed;
}

void VersionedSparseAbstractInterpretation::reportTelemetry() const
{
    std::size_t phiCount = 0;
    for (const auto& [node, phis] : phiVersions_)
    {
        (void)node;
        phiCount += phis.size();
    }
    SVFUtil::outs() << "AE_D3 versions=" << versionVariables_.size()
                    << " store_versions=" << storeVersionIds_.size()
                    << " phi_versions=" << phiCount
                    << " cycle_head_drops=" << cycleHeadDrops_
                    << " havoc_stores=" << havocStores_.size()
                    << " external_invalidations=" << externalInvalidations_
                    << " versioned_nodes=" << versionOut_.size() << '\n';
}
