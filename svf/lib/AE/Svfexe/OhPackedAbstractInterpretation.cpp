//===- OhPackedAbstractInterpretation.cpp -- Oh Packed Abstract Interpretation --------===//
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

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AE/Svfexe/OhPackedAbstractInterpretation.h"
#include "Util/Options.h"
#include <chrono>
#include <functional>
#include <stdexcept>

using namespace SVF;

void OhPackedAbstractInterpretation::initializeExecutionPolicy()
{
    const auto start = std::chrono::steady_clock::now();
    Defs all;
    bool mutated=false;
    // Singleton variables flow in the existing dense Box snapshot, with no
    // Octagon definition slots, singleton containers or per-state type tags.
    const SVF::PackedRelationalDomain schema(relationalPacking_);
    for (auto p:schema.relationalPackIndices()) all.insert(p);
    std::map<const ICFGNode*, const ICFGNode*> predecessor;
    for (auto it=icfg->begin(); it!=icfg->end(); ++it)
    {
        const auto* node=it->second;
        bool barrier = !SVFUtil::isa<IntraICFGNode>(node) || node->getInEdges().size()!=1;
        if (!barrier)
        {
            const auto* edge=SVFUtil::dyn_cast<IntraCFGEdge>(*node->getInEdges().begin());
            barrier = !edge || edge->getCondition();
            if (!barrier) predecessor[node]=edge->getSrcNode();
        }
        Defs defs;
        for (const SVFStmt* stmt:node->getSVFStmts())
        {
            const ValVar* target=nullptr;
            if (const auto* binary=SVFUtil::dyn_cast<BinaryOPStmt>(stmt)) target=binary->getRes();
            else if (const auto* cmp=SVFUtil::dyn_cast<CmpStmt>(stmt)) target=cmp->getRes();
            else if (const auto* copy=SVFUtil::dyn_cast<CopyStmt>(stmt))
                target=SVFUtil::dyn_cast<ValVar>(copy->getLHSVar());
            else if (SVFUtil::isa<BranchStmt>(stmt)) continue;
            else
            {
                barrier=true;
                continue;
            }
            if (!target || !adapter_.contains(*target))
            {
                barrier=true;
                continue;
            }
            const auto variable=adapter_.variable(*target);
            for (auto p:all)
            {
                const auto& pack=relationalPacking_->at(p);
                if (std::binary_search(pack.begin(),pack.end(),variable)) defs.insert(p);
            }
        }
        if (!barrier && !defs.empty() && !mutated &&
                std::getenv("SVF_AE_OH_DROP_DEFINITION"))
        {
            defs.clear();
            mutated=true;
        }
        definitions_[node]=barrier?all:std::move(defs);
    }
    // Single-predecessor identity chains have a unique reaching definition.
    // Every join/edge transformer is an explicit all-pack definition. Break
    // any residual pure single-predecessor cycle conservatively before routing.
    for (auto it=icfg->begin(); it!=icfg->end(); ++it)
    {
        std::set<const ICFGNode*> seen;
        auto* n=it->second;
        const ICFGNode* cursor=n;
        while (predecessor.count(cursor) && definitions_.at(cursor)!=all)
        {
            if (!seen.insert(cursor).second)
            {
                definitions_[cursor]=all;
                break;
            }
            cursor=predecessor.at(cursor);
        }
    }
    for (auto it=icfg->begin(); it!=icfg->end(); ++it)
    {
        const auto* node=it->second;
        auto& routes=routes_[node];
        for (auto p:all)
        {
            const ICFGNode* cursor=node;
            while (!definitions_.at(cursor).count(p)) cursor=predecessor.at(cursor);
            routes.emplace(p,cursor);
        }
    }
    graphSeconds_=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::size_t slots=0;
    for (const auto& item:definitions_) slots+=item.second.size();
    SVFUtil::outs()<<"AE_OH_GRAPH nodes="<<definitions_.size()<<" packs="<<all.size()
                   <<" def_slots="<<slots<<" dense_slots="<<definitions_.size()*all.size()
                   <<" uses=all-packs auxiliary=dense graph_s="<<graphSeconds_<<'\n';
}

const OhPackedAbstractInterpretation::Pack& OhPackedAbstractInterpretation::definition(
    const ICFGNode* node, std::size_t p) const
{
    const auto raw=stateTrace_.find(node);
    if (raw==stateTrace_.end()) throw std::runtime_error("Oh missing reaching definition state");
    if (raw->second.numerical().isDomain<SVF::PackedRelationalDomain>())
        return static_cast<const SVF::PackedRelationalDomain&>(raw->second.numerical()).packState(p);
    const auto stored=storedPacks_.find(node);
    if (stored==storedPacks_.end() || !stored->second.count(p))
        throw std::runtime_error("Oh missing declared pack definition");
    return stored->second.at(p);
}

AbstractInterpretation::State OhPackedAbstractInterpretation::reconstruct(const ICFGNode* node) const
{
    State result=stateTrace_.at(node);
    if (result.numerical().isDomain<SVF::PackedRelationalDomain>()) return result;
    if (!result.numerical().isDomain<SVF::BoxDomain>()) throw std::logic_error("Oh storage kind");
    auto domain=std::make_unique<SVF::PackedRelationalDomain>(relationalPacking_);
    std::vector<Pack> packs;
    const auto& route=routes_.at(node);
    for (const auto& [p,source]:route) packs.push_back(definition(source,p));
    domain->restoreRelationalComponents(static_cast<const SVF::BoxDomain&>(result.numerical()),std::move(packs));
    result.replaceNumericalStorage(std::move(domain));
    return result;
}

const AbstractInterpretation::State& OhPackedAbstractInterpretation::state(const ICFGNode* node) const
{
    if (recheckStates_) return AbstractInterpretation::state(node);
    const State& raw=stateTrace_.at(node);
    if (raw.numerical().isDomain<SVF::PackedRelationalDomain>()) return raw;
    // No cache reuse across writes. References are consumed within one shared
    // transfer; owned clones are used across recursive calls and loop steps.
    auto hit=readCache_.find(node);
    if (hit==readCache_.end()) hit=readCache_.emplace(node,std::make_unique<State>(reconstruct(node))).first;
    peakReadCacheNodes_=std::max(peakReadCacheNodes_,readCache_.size()+retiredReadCache_.size());
    return *hit->second;
}

void OhPackedAbstractInterpretation::invalidateReadCache()
{
    for (auto& item:readCache_) retiredReadCache_.push_back(std::move(item.second));
    readCache_.clear();
}

AbstractInterpretation::State& OhPackedAbstractInterpretation::ensureState(const ICFGNode* node)
{
    if (recheckStates_ && recheckStates_->count(node)) return AbstractInterpretation::ensureState(node);
    invalidateReadCache();
    auto hit=stateTrace_.find(node);
    if (hit==stateTrace_.end()) hit=stateTrace_.emplace(node,topState()).first;
    else if (!hit->second.numerical().isDomain<SVF::PackedRelationalDomain>())
    {
        State full=reconstruct(node);
        hit->second=std::move(full);
        ++restored_;
    }
    expanded_.insert(node);
    return hit->second;
}

void OhPackedAbstractInterpretation::compact(const ICFGNode* node)
{
    auto it=stateTrace_.find(node);
    if (it==stateTrace_.end() || !it->second.numerical().isDomain<SVF::PackedRelationalDomain>()) return;
    auto& numerical=static_cast<SVF::PackedRelationalDomain&>(it->second.numerical());
    auto& saved=storedPacks_[node];
    const auto& defs=definitions_.at(node);
    for (auto p:numerical.relationalPackIndices())
    {
        if (defs.count(p)) saved.insert_or_assign(p,numerical.packState(p));
        else
        {
            ++omittedChecks_;
            if (numerical.packState(p).isEquivalentTo(definition(routes_.at(node)[p],p))!=SVF::CheckResult::True)
                throw std::runtime_error("Oh undeclared pack write at node "+std::to_string(node->getId())+
                                         " pack "+std::to_string(p));
        }
    }
    auto fallback=std::make_unique<SVF::BoxDomain>(numerical.fallbackState());
    it->second.replaceNumericalStorage(std::move(fallback));
    expanded_.erase(node);
    readCache_.clear();
    retiredReadCache_.clear();
}

void OhPackedAbstractInterpretation::compactBorrowed()
{
    const auto pending=expanded_;
    for (const auto* node:pending) if (!active_.count(node)) compact(node);
}
void OhPackedAbstractInterpretation::beginAbstractState(const ICFGNode* node)
{
    ++active_[node];
    ensureState(node);
}
void OhPackedAbstractInterpretation::finalizeAbstractState(const ICFGNode* node)
{
    auto it=active_.find(node);
    if (it!=active_.end() && --it->second==0) active_.erase(it);
    if (!active_.count(node)) compact(node);
    compactBorrowed();
}
bool OhPackedAbstractInterpretation::mergeStatesFromPredecessors(const ICFGNode* node)
{
    compactBorrowed();
    readCache_.clear();
    retiredReadCache_.clear();
    const bool merged=AbstractInterpretation::mergeStatesFromPredecessors(node);
    if (merged) expanded_.insert(node);
    return merged;
}
void OhPackedAbstractInterpretation::resetAbstractState(const ICFGNode* node)
{
    readCache_.clear();
    AbstractInterpretation::resetAbstractState(node);
    expanded_.insert(node);
}
void OhPackedAbstractInterpretation::copyAbstractState(const ICFGNode* source,const ICFGNode* destination)
{
    State copy=state(source);
    readCache_.clear();
    stateTrace_.insert_or_assign(destination,std::move(copy));
    expanded_.insert(destination);
}
void OhPackedAbstractInterpretation::handleGlobalNode()
{
    AbstractInterpretation::handleGlobalNode();
    compact(icfg->getGlobalICFGNode());
}
bool OhPackedAbstractInterpretation::widenCycleState(const SVF::AbstractDomain& a,
        const SVF::AbstractDomain& b,const ICFGCycleWTO* cycle)
{
    const bool result=AbstractInterpretation::widenCycleState(a,b,cycle);
    compact(cycle->head()->getICFGNode());
    return result;
}
bool OhPackedAbstractInterpretation::narrowCycleState(const SVF::AbstractDomain& a,
        const SVF::AbstractDomain& b,const ICFGCycleWTO* cycle)
{
    const bool result=AbstractInterpretation::narrowCycleState(a,b,cycle);
    compact(cycle->head()->getICFGNode());
    return result;
}
void OhPackedAbstractInterpretation::runOnModule()
{
    AbstractInterpretation::runOnModule();
    compactBorrowed();
    if (std::getenv("SVF_AE_OH_CACHE_CHECK")) checkCacheInvalidation();
    std::size_t slots=0;
    for (const auto& item:storedPacks_) slots+=item.second.size();
    SVFUtil::outs()<<"AE_OH_STORAGE pack_slots="<<slots<<" flow_states="<<stateTrace_.size()
                   <<" restored="<<restored_<<" identity_checks="<<omittedChecks_
                   <<" peak_read_cache_nodes="<<peakReadCacheNodes_
                   <<" peak_read_cache_pack_slots="<<peakReadCacheNodes_*std::count_if(relationalPacking_->begin(),relationalPacking_->end(),
                           [](const auto& pack)
    {
        return pack.size()>1;
    })
            <<" post_replay=dense"<<'\n';
    readCache_.clear();
    retiredReadCache_.clear();
}

void OhPackedAbstractInterpretation::checkCacheInvalidation()
{
    // A focused accessor-contract regression, after analysis and Post. Save
    // and restore the source exactly; no detector result is produced here.
    for (const auto& item:routes_)
    {
        const auto* node=item.first;
        if (!stateTrace_.count(node)) continue;
        for (const auto& [p,source]:item.second)
        {
            if (source==node || !stateTrace_.count(source)) continue;
            compact(node);
            compact(source);
            State original=state(source);
            const State& oldRead=state(node);
            auto replacement=std::make_unique<SVF::PackedRelationalDomain>(relationalPacking_);
            replacement->assign(relationalPacking_->at(p).front(),SVF::LinearExpression(SVF::Rational(42)));
            const Pack expected=replacement->packState(p);
            ensureState(source).replaceNumericalStorage(std::move(replacement));
            const auto& observed=static_cast<const SVF::PackedRelationalDomain&>(state(node).numerical());
            // A bottom fallback may normalize the whole product; select a
            // reachable sample for which the old Box state is non-bottom.
            const bool good=observed.packState(p).isEquivalentTo(expected)==SVF::CheckResult::True;
            (void)oldRead.numerical().isBottom(); // old reference remains valid
            stateTrace_.insert_or_assign(source,std::move(original));
            invalidateReadCache();
            compact(source);
            if (!good) throw std::runtime_error("Oh cache invalidation regression");
            SVFUtil::outs()<<"AE_OH_CACHE_CHECK Pass\n";
            return;
        }
    }
    // With no relational locations there is no reaching-pack route. Exercise
    // the actual dense Box reconstruction cache at the same node instead.
    const SVF::PackedRelationalDomain schema(relationalPacking_);
    if(schema.materializedPackCount()==0)
        for(const auto& entry:stateTrace_)
        {
            const auto* node=entry.first;
            State original=state(node);
            if(original.numerical().isBottom()) continue;
            const State& oldRead=state(node);
            auto replacement=std::make_unique<SVF::PackedRelationalDomain>(relationalPacking_);
            if(relationalPacking_->empty()) break;
            const auto variable=relationalPacking_->front().front();
            replacement->assign(variable,SVF::LinearExpression(SVF::Rational(42)));
            ensureState(node).replaceNumericalStorage(std::move(replacement));
            const bool good=state(node).numerical().bound(variable)==SVF::Interval::singleton(SVF::Rational(42));
            (void)oldRead.numerical().isBottom();
            stateTrace_.insert_or_assign(node,std::move(original));
            invalidateReadCache();
            compact(node);
            if(!good) throw std::runtime_error("Oh singleton Box cache invalidation regression");
            SVFUtil::outs()<<"AE_OH_CACHE_CHECK Pass singleton-box-dense\n";
            return;
        }
    throw std::runtime_error("Oh cache regression found no nontrivial route");
}
