//===- PackedRelationalDomain.cpp -- Packed Relational Domain --------===//
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
#include "AE/Core/PackedRelationalDomain.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <mutex>
#include <limits>
namespace SVF
{
namespace
{
OctagonConfig packedConfig()
{
    OctagonConfig c;
    c.storage=OctagonStorageKind::ComponentDense;
    return c;
}
std::set<Variable> vars(const LinearExpression& e)
{
    std::set<Variable> s;
    for(const auto& t:e.terms()) s.insert(t.first);
    return s;
}
std::set<Variable> vars(const TreeExpression& e)
{
    if(e.kind()==TreeExpression::Kind::Variable) return {e.variable()};
    if(e.kind()==TreeExpression::Kind::Constant) return {};
    auto s=vars(e.lhs());
    if(e.kind()==TreeExpression::Kind::Binary)
    {
        auto r=vars(e.rhs());
        s.insert(r.begin(),r.end());
    }
    return s;
}
void restrictBound(OctagonDomain& d,Variable v,const Interval& i)
{
    if(i.isBottom())
    {
        d=OctagonDomain::bottom(packedConfig());
        return;
    }
    if(i.lower().isFinite()) d.assume(LinearConstraint(LinearExpression(v)-LinearExpression(i.lower().value()),
                i.lower().isStrict()?ConstraintKind::GreaterThan:ConstraintKind::GreaterEqual));
    if(i.upper().isFinite()) d.assume(LinearConstraint(LinearExpression(v)-LinearExpression(i.upper().value()),
                i.upper().isStrict()?ConstraintKind::LessThan:ConstraintKind::LessEqual));
}
void restrictBound(BoxDomain& d,Variable v,const Interval& i)
{
    if(i.isBottom())
    {
        d=BoxDomain::bottom();
        return;
    }
    if(i.lower().isFinite()) d.assume(LinearConstraint(LinearExpression(v)-LinearExpression(i.lower().value()),
                i.lower().isStrict()?ConstraintKind::GreaterThan:ConstraintKind::GreaterEqual));
    if(i.upper().isFinite()) d.assume(LinearConstraint(LinearExpression(v)-LinearExpression(i.upper().value()),
                i.upper().isStrict()?ConstraintKind::LessThan:ConstraintKind::LessEqual));
}
}
std::shared_ptr<const PackedRelationalDomain::Layout> PackedRelationalDomain::layoutFor(
    const std::shared_ptr<const Packing>& packing)
{
    if(!packing) throw std::invalid_argument("null packing");
    // The schema owns routing, not each abstract state. A weak cache neither
    // retains old campaigns nor aliases a recycled packing address.
    struct Entry
    {
        std::weak_ptr<const Packing> packing;
        std::shared_ptr<const Layout> layout;
    };
    static std::map<const Packing*,Entry> cache;
    static std::mutex mutex;
    std::lock_guard<std::mutex> guard(mutex);
    if(auto found=cache.find(packing.get()); found!=cache.end())
        if(found->second.packing.lock()==packing)
            return found->second.layout;
    auto layout=std::make_shared<Layout>();
    for(std::size_t logical=0; logical<packing->size(); ++logical)
    {
        const auto& pack=packing->at(logical);
        if(pack.empty() || !std::is_sorted(pack.begin(),pack.end()) ||
                std::adjacent_find(pack.begin(),pack.end())!=pack.end())
            throw std::invalid_argument("pack must be nonempty, sorted and unique");
        for(auto v:pack) if(v.type().kind==NumericKind::IEEEFloat)
                throw std::invalid_argument("IEEE values must use the Box fallback");
        if(pack.size()==1)
        {
            layout->singletons.insert(pack.front());
            layout->logicalToPhysical.push_back(std::numeric_limits<std::size_t>::max());
        }
        else
        {
            const auto physical=layout->physicalToLogical.size();
            layout->logicalToPhysical.push_back(physical);
            layout->physicalToLogical.push_back(logical);
            for(auto v:pack) layout->owners[v].push_back(physical);
        }
    }
    for(auto it=cache.begin(); it!=cache.end();)
        if(it->second.packing.expired()) it=cache.erase(it);
        else ++it;
    cache[packing.get()]= {packing,layout};
    return layout;
}
PackedRelationalDomain::PackedRelationalDomain(std::shared_ptr<const Packing> p,bool bottom)
    :packing_(std::move(p)),layout_(layoutFor(packing_)),box_(bottom?BoxDomain::bottom():BoxDomain::top())
{
    for(auto logical:layout_->physicalToLogical)
    {
        (void)logical;
        packs_.push_back(bottom?OctagonDomain::bottom(packedConfig()):OctagonDomain::top(packedConfig()));
    }
}
std::unique_ptr<AbstractDomain> PackedRelationalDomain::clone() const
{
    return std::make_unique<PackedRelationalDomain>(*this);
}
void PackedRelationalDomain::restoreComponents(BoxDomain fallback, std::vector<OctagonDomain> packs)
{
    if(packs.size()!=packing_->size()) throw std::invalid_argument("packed restore size mismatch");
    std::vector<OctagonDomain> relational;
    for(std::size_t p=0; p<packs.size(); ++p)
        if(isRelationalPack(p)) relational.push_back(std::move(packs[p]));
        else restrictBound(fallback,packing_->at(p).front(),packs[p].bound(packing_->at(p).front()));
    restoreRelationalComponents(std::move(fallback),std::move(relational));
}
void PackedRelationalDomain::restoreRelationalComponents(BoxDomain fallback,std::vector<OctagonDomain> packs)
{
    if(packs.size()!=layout_->physicalToLogical.size()) throw std::invalid_argument("relational restore size mismatch");
    box_=std::move(fallback);
    packs_=std::move(packs);
    normalizeBottom();
}
bool PackedRelationalDomain::isRelationalPack(std::size_t logical) const
{
    return layout_->logicalToPhysical.at(logical)!=std::numeric_limits<std::size_t>::max();
}
const OctagonDomain& PackedRelationalDomain::packState(std::size_t logical) const
{
    if(!isRelationalPack(logical)) throw std::invalid_argument("singleton pack is stored in global Box");
    return packs_.at(layout_->logicalToPhysical.at(logical));
}
const std::vector<std::size_t>& PackedRelationalDomain::relationalPackIndices() const
{
    return layout_->physicalToLogical;
}
bool PackedRelationalDomain::singleton(Variable v) const
{
    return layout_->singletons.count(v)!=0;
}
bool PackedRelationalDomain::contains(std::size_t p,Variable v) const
{
    const auto& pack=packing_->at(layout_->physicalToLogical.at(p));
    return std::binary_search(pack.begin(),pack.end(),v);
}
template<class Operation>
void PackedRelationalDomain::updateSingletons(BoxDomain& next,const std::set<Variable>& targets,
        const std::set<Variable>& reads,Operation operation) const
{
    if(packs_.empty())return;
    bool touched=false;
    for(auto v:targets) touched|=singleton(v);
    if(!touched) return;
    bool external=false;
    for(auto v:reads) external|=layout_->owners.count(v)!=0;
    if(!external) return; // The ordinary Box operation already has every input bound.
    auto projected=box_;
    for(auto v:reads) restrictBound(projected,v,bound(v));
    operation(projected);
    for(auto v:targets) if(singleton(v))
        {
            const auto interval=projected.bound(v);
            next.forget(v);
            restrictBound(next,v,interval);
        }
}
void PackedRelationalDomain::normalizeBottom()
{
    if(!isBottom()) return;
    box_=BoxDomain::bottom();
    for(auto& p:packs_) p=OctagonDomain::bottom(packedConfig());
}
void PackedRelationalDomain::importOutside(OctagonDomain& d,std::size_t p,const std::set<Variable>& reads) const
{
    for(auto v:reads) if(!contains(p,v)) restrictBound(d,v,bound(v));
}
void PackedRelationalDomain::update(const std::set<Variable>& targets,const std::set<Variable>& reads,
                                    const std::function<void(OctagonDomain&,std::size_t)>& op)
{
    // All replacements are computed against one immutable pre-state.
    std::vector<std::pair<std::size_t,OctagonDomain>> next;
    std::set<std::size_t> affected;
    if(targets.empty()) for(std::size_t p=0; p<packs_.size(); ++p) affected.insert(p);
    else for(auto v:targets) if(auto it=layout_->owners.find(v); it!=layout_->owners.end())
                affected.insert(it->second.begin(),it->second.end());
    for(auto p:affected)
    {
        auto d=packs_[p];
        importOutside(d,p,reads);
        op(d,p);
        d.project(packing_->at(layout_->physicalToLogical[p]));
        next.emplace_back(p,std::move(d));
    }
    for(auto& pair:next) packs_[pair.first]=std::move(pair.second);
}
void PackedRelationalDomain::assign(Variable v,const LinearExpression& e)
{
    if(isBottom()) return;
    if(packs_.empty())
    {
        box_.assign(v,e);
        return;
    }
    auto b=box_;
    b.assign(v,e);
    updateSingletons(b, {v},vars(e),[&](auto& d)
    {
        d.assign(v,e);
    });
    update({v},vars(e),[&](auto& d,std::size_t)
    {
        d.assign(v,e);
    });
    box_=std::move(b);
    normalizeBottom();
}
void PackedRelationalDomain::assign(Variable v,const TreeExpression& e)
{
    if(isBottom()) return;
    if(packs_.empty())
    {
        box_.assign(v,e);
        return;
    }
    auto b=box_;
    b.assign(v,e);
    updateSingletons(b, {v},vars(e),[&](auto& d)
    {
        d.assign(v,e);
    });
    update({v},vars(e),[&](auto& d,std::size_t)
    {
        d.assign(v,e);
    });
    box_=std::move(b);
    normalizeBottom();
}
void PackedRelationalDomain::assignParallel(const LinearAssignmentList& assignments)
{
    if(isBottom() || assignments.empty()) return;
    if(packs_.empty())
    {
        box_.assignParallel(assignments);
        return;
    }
    auto b=box_;
    b.assignParallel(assignments);
    std::set<Variable> targets,reads;
    for(const auto& a:assignments)
    {
        targets.insert(a.target);
        auto r=vars(a.expression);
        reads.insert(r.begin(),r.end());
    }
    updateSingletons(b,targets,reads,[&](auto& d)
    {
        d.assignParallel(assignments);
    });
    update(targets,reads,[&](auto& d,std::size_t p)
    {
        LinearAssignmentList local;
        for(const auto& a:assignments) if(contains(p,a.target)) local.push_back(a);
        d.assignParallel(local);
    });
    box_=std::move(b);
    normalizeBottom();
}
void PackedRelationalDomain::substitute(Variable v,const LinearExpression& e)
{
    substituteParallel({{v,e}});
}
void PackedRelationalDomain::substituteParallel(const LinearAssignmentList& assignments)
{
    if(isBottom() || assignments.empty()) return;
    box_.substituteParallel(assignments);
    for(std::size_t p=0; p<packs_.size(); ++p)
    {
        // Backward transformers cannot import post-state bounds as pre-state
        // assumptions. Forget cross-boundary outputs before exact local substitution.
        LinearAssignmentList local;
        for(const auto& a:assignments)
        {
            if(!contains(p,a.target)) continue;
            bool inside=true;
            for(auto v:vars(a.expression)) inside&=contains(p,v);
            if(inside) local.push_back(a);
            else packs_[p].forget(a.target);
        }
        packs_[p].substituteParallel(local);
    }
    normalizeBottom();
}
void PackedRelationalDomain::assume(const LinearConstraint& c)
{
    if(isBottom()) return;
    if(packs_.empty())
    {
        box_.assume(c);
        return;
    }
    auto b=box_;
    b.assume(c);
    auto reads=vars(c.expression());
    updateSingletons(b,reads,reads,[&](auto& d)
    {
        d.assume(c);
    });
    update(reads,reads,[&](auto& d,std::size_t)
    {
        d.assume(c);
    });
    box_=std::move(b);
    normalizeBottom();
}
void PackedRelationalDomain::assume(const TreeConstraint& c)
{
    if(isBottom()) return;
    if(packs_.empty())
    {
        box_.assume(c);
        return;
    }
    auto b=box_;
    b.assume(c);
    auto reads=vars(c.expression());
    updateSingletons(b,reads,reads,[&](auto& d)
    {
        d.assume(c);
    });
    update(reads,reads,[&](auto& d,std::size_t)
    {
        d.assume(c);
    });
    box_=std::move(b);
    normalizeBottom();
}
void PackedRelationalDomain::assumeAll(const LinearConstraintSet& cs)
{
    for(const auto& c:cs) assume(c);
}
void PackedRelationalDomain::forget(Variable v)
{
    box_.forget(v);
    if(auto it=layout_->owners.find(v); it!=layout_->owners.end()) for(auto p:it->second) packs_[p].forget(v);
    normalizeBottom();
}
void PackedRelationalDomain::project(const std::vector<Variable>& retained)
{
    box_.project(retained);
    for(auto& p:packs_) p.project(retained);
    normalizeBottom();
}
void PackedRelationalDomain::expand(Variable v,const std::vector<Variable>& copies)
{
    if(packs_.empty())
    {
        box_.expand(v,copies);
        return;
    }
    // Expansion duplicates a coordinate's constraints, without asserting that
    // the new coordinates equal the source or one another.
    auto b=box_;
    b.expand(v,copies);
    std::set<Variable> targets(copies.begin(),copies.end());
    if(targets.empty()) return;
    updateSingletons(b,targets, {v},[&](auto& d)
    {
        d.expand(v,copies);
    });
    update(targets, {v},[&](auto& d,std::size_t p)
    {
        std::vector<Variable> local;
        for(auto copy:copies) if(contains(p,copy)) local.push_back(copy);
        d.expand(v,local);
    });
    box_=std::move(b);
    normalizeBottom();
}
void PackedRelationalDomain::fold(Variable target,const std::vector<Variable>& folded)
{
    box_.fold(target,folded);
    for(std::size_t p=0; p<packs_.size(); ++p)
    {
        bool inside=contains(p,target);
        for(auto v:folded) inside&=contains(p,v);
        if(inside) packs_[p].fold(target,folded);
        else
        {
            packs_[p].forget(target);
            for(auto v:folded) packs_[p].forget(v);
            if(contains(p,target)) restrictBound(packs_[p],target,box_.bound(target));
        }
    }
    normalizeBottom();
}
CheckResult PackedRelationalDomain::entails(const LinearConstraint& c) const
{
    if(isBottom() || box_.entails(c)==CheckResult::True) return CheckResult::True;
    for(const auto& p:packs_) if(p.entails(c)==CheckResult::True) return CheckResult::True;
    return CheckResult::Unknown;
}
Interval PackedRelationalDomain::bound(Variable v) const
{
    if(isBottom()) return Interval::bottom();
    auto b=box_.bound(v);
    if(auto it=layout_->owners.find(v); it!=layout_->owners.end())
        for(auto p:it->second) b.meetWith(packs_[p].bound(v));
    return b;
}
Interval PackedRelationalDomain::bound(const LinearExpression& e) const
{
    if(isBottom()) return Interval::bottom();
    auto b=box_.bound(e);
    for(const auto& p:packs_) b.meetWith(p.bound(e));
    return b;
}
std::vector<Variable> PackedRelationalDomain::supportVariables() const
{
    if(packs_.empty()) return box_.supportVariables();
    const auto b=box_.supportVariables();
    std::set<Variable> s(b.begin(),b.end());
    for(const auto& p:packs_)
    {
        auto v=p.supportVariables();
        s.insert(v.begin(),v.end());
    }
    return {s.begin(),s.end()};
}
LinearConstraintSet PackedRelationalDomain::toConstraints() const
{
    auto cs=box_.toConstraints();
    for(const auto& p:packs_)
    {
        auto more=p.toConstraints();
        cs.insert(cs.end(),more.begin(),more.end());
    }
    return cs;
}
void PackedRelationalDomain::close()
{
    box_.close();
    for(auto& p:packs_) p.close();
    normalizeBottom();
}
void PackedRelationalDomain::canonicalize()
{
    box_.canonicalize();
    for(auto& p:packs_) p.canonicalize();
    normalizeBottom();
}
bool PackedRelationalDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    return other.isDomain<PackedRelationalDomain>() &&
           (packing_==static_cast<const PackedRelationalDomain&>(other).packing_ ||
            *packing_==*static_cast<const PackedRelationalDomain&>(other).packing_);
}
void PackedRelationalDomain::joinDomain(const AbstractDomain& other)
{
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    box_.joinWith(o.box_);
    for(std::size_t p=0; p<packs_.size(); ++p) packs_[p].joinWith(o.packs_[p]);
    normalizeBottom();
}
void PackedRelationalDomain::meetDomain(const AbstractDomain& other)
{
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    box_.meetWith(o.box_);
    for(std::size_t p=0; p<packs_.size(); ++p) packs_[p].meetWith(o.packs_[p]);
    normalizeBottom();
}
void PackedRelationalDomain::widenDomain(const AbstractDomain& other)
{
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    box_.widenWith(o.box_);
    for(std::size_t p=0; p<packs_.size(); ++p) packs_[p].widenWith(o.packs_[p]);
    normalizeBottom();
}
void PackedRelationalDomain::narrowDomain(const AbstractDomain& other)
{
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    box_.narrowWith(o.box_);
    for(std::size_t p=0; p<packs_.size(); ++p) packs_[p].narrowWith(o.packs_[p]);
    normalizeBottom();
}
bool PackedRelationalDomain::isBottomDomain() const
{
    if(box_.isBottom()) return true;
    for(const auto& p:packs_) if(p.isBottom()) return true;
    return false;
}
bool PackedRelationalDomain::isTopDomain() const
{
    if(!box_.isTop()) return false;
    for(const auto& p:packs_) if(!p.isTop()) return false;
    return true;
}
bool PackedRelationalDomain::leqDomain(const AbstractDomain& other) const
{
    if(isBottom()) return true;
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    if(o.isBottom()) return false;
    // Fast sufficient test on the stored product components.
    bool componentwise = box_.isSubsetOf(o.box_) == CheckResult::True;
    for(std::size_t p=0; componentwise && p<packs_.size(); ++p)
        componentwise = packs_[p].isSubsetOf(o.packs_[p]) == CheckResult::True;
    if(componentwise) return true;

    // A state can store a unary consequence only in one overlapping pack.
    // Import bounds implied by the entire LHS into temporary LHS components
    // before checking. These consequences preserve its concretization; the
    // original carriers and the RHS are untouched. One reduction pass is a
    // sufficient inclusion check, not a complete relational decision rule.
    for(Variable v:o.box_.supportVariables())
        if(!bound(v).isSubsetOf(o.box_.bound(v))) return false;
    for(std::size_t p=0; p<packs_.size(); ++p)
    {
        if(packs_[p].isSubsetOf(o.packs_[p])==CheckResult::True) continue;
        auto reduced = packs_[p];
        for(Variable v:packing_->at(layout_->physicalToLogical[p])) restrictBound(reduced,v,bound(v));
        if(reduced.isSubsetOf(o.packs_[p])!=CheckResult::True) return false;
    }
    return true;
}
std::string PackedRelationalDomain::domainToString() const
{
    std::ostringstream s;
    s<<"PackedOctagon("<<box_.toString();
    for(const auto& p:packs_) s<<","<<p.toString();
    s<<")";
    return s.str();
}
std::vector<Variable> PackedRelationalDomain::relationalClosureState(const std::vector<Variable>& seeds) const
{
    // Conservative, state-independent envelope for the EXISTING Semi-Sparse
    // reconstruction. It is not the standalone Oh dependency graph.
    std::set<Variable> reached(seeds.begin(),seeds.end());
    bool changed=true;
    while(changed)
    {
        changed=false;
        for(auto logical:layout_->physicalToLogical)
        {
            const auto& pack=packing_->at(logical);
            bool touches=false;
            for(auto v:pack) touches|=reached.count(v)!=0;
            if(touches) for(auto v:pack) changed|=reached.insert(v).second;
        }
    }
    return {reached.begin(),reached.end()};
}
}
