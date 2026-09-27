// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AE/Core/PackedRelationalDomain.h"
#include <algorithm>
#include <sstream>
#include <stdexcept>
namespace SVF::AbstractDomain
{
namespace
{
OctagonConfig packedConfig() { OctagonConfig c; c.storage=OctagonStorageKind::ComponentDense; return c; }
std::set<Variable> vars(const LinearExpression& e)
{ std::set<Variable> s; for(const auto& t:e.terms()) s.insert(t.first); return s; }
std::set<Variable> vars(const TreeExpression& e)
{
    if(e.kind()==TreeExpression::Kind::Variable) return {e.variable()};
    if(e.kind()==TreeExpression::Kind::Constant) return {};
    auto s=vars(e.lhs());
    if(e.kind()==TreeExpression::Kind::Binary) { auto r=vars(e.rhs()); s.insert(r.begin(),r.end()); }
    return s;
}
void restrictBound(OctagonDomain& d,Variable v,const Interval& i)
{
    if(i.isBottom()) { d=OctagonDomain::bottom(packedConfig()); return; }
    if(i.lower().isFinite()) d.assume(LinearConstraint(LinearExpression(v)-LinearExpression(i.lower().value()),
        i.lower().isStrict()?ConstraintKind::GreaterThan:ConstraintKind::GreaterEqual));
    if(i.upper().isFinite()) d.assume(LinearConstraint(LinearExpression(v)-LinearExpression(i.upper().value()),
        i.upper().isStrict()?ConstraintKind::LessThan:ConstraintKind::LessEqual));
}
}
PackedRelationalDomain::PackedRelationalDomain(std::shared_ptr<const Packing> p,bool bottom)
    :packing_(std::move(p)),box_(bottom?BoxDomain::bottom():BoxDomain::top())
{
    if(!packing_) throw std::invalid_argument("null packing");
    for(const auto& pack:*packing_)
    {
        if(pack.empty() || !std::is_sorted(pack.begin(),pack.end()) ||
           std::adjacent_find(pack.begin(),pack.end())!=pack.end())
            throw std::invalid_argument("pack must be nonempty, sorted and unique");
        for(auto v:pack) if(v.type().kind==NumericKind::IEEEFloat)
            throw std::invalid_argument("IEEE values must use the Box fallback");
        packs_.push_back(bottom?OctagonDomain::bottom(packedConfig()):OctagonDomain::top(packedConfig()));
    }
}
std::unique_ptr<AbstractDomain> PackedRelationalDomain::clone() const
{ return std::make_unique<PackedRelationalDomain>(*this); }
void PackedRelationalDomain::restoreComponents(BoxDomain fallback, std::vector<OctagonDomain> packs)
{
    if(packs.size()!=packing_->size()) throw std::invalid_argument("packed restore size mismatch");
    box_=std::move(fallback); packs_=std::move(packs); normalizeBottom();
}
bool PackedRelationalDomain::contains(std::size_t p,Variable v) const
{ const auto& pack=packing_->at(p); return std::binary_search(pack.begin(),pack.end(),v); }
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
    for(std::size_t p=0;p<packs_.size();++p)
    {
        bool touched=targets.empty();
        for(auto v:targets) touched|=contains(p,v);
        if(!touched) continue;
        auto d=packs_[p]; importOutside(d,p,reads); op(d,p); d.project(packing_->at(p));
        next.emplace_back(p,std::move(d));
    }
    for(auto& pair:next) packs_[pair.first]=std::move(pair.second);
}
void PackedRelationalDomain::assign(Variable v,const LinearExpression& e)
{
    if(isBottom()) return;
    auto b=box_; b.assign(v,e);
    update({v},vars(e),[&](auto& d,std::size_t){d.assign(v,e);});
    box_=std::move(b); normalizeBottom();
}
void PackedRelationalDomain::assign(Variable v,const TreeExpression& e)
{
    if(isBottom()) return;
    auto b=box_; b.assign(v,e);
    update({v},vars(e),[&](auto& d,std::size_t){d.assign(v,e);});
    box_=std::move(b); normalizeBottom();
}
void PackedRelationalDomain::assignParallel(const LinearAssignmentList& assignments)
{
    if(isBottom() || assignments.empty()) return;
    auto b=box_; b.assignParallel(assignments);
    std::set<Variable> targets,reads;
    for(const auto& a:assignments) { targets.insert(a.target); auto r=vars(a.expression); reads.insert(r.begin(),r.end()); }
    update(targets,reads,[&](auto& d,std::size_t p){
        LinearAssignmentList local;
        for(const auto& a:assignments) if(contains(p,a.target)) local.push_back(a);
        d.assignParallel(local);
    });
    box_=std::move(b); normalizeBottom();
}
void PackedRelationalDomain::substitute(Variable v,const LinearExpression& e)
{ substituteParallel({{v,e}}); }
void PackedRelationalDomain::substituteParallel(const LinearAssignmentList& assignments)
{
    if(isBottom() || assignments.empty()) return;
    box_.substituteParallel(assignments);
    for(std::size_t p=0;p<packs_.size();++p)
    {
        // Backward transformers cannot import post-state bounds as pre-state
        // assumptions. Forget cross-boundary outputs before exact local substitution.
        LinearAssignmentList local;
        for(const auto& a:assignments)
        {
            if(!contains(p,a.target)) continue;
            bool inside=true; for(auto v:vars(a.expression)) inside&=contains(p,v);
            if(inside) local.push_back(a); else packs_[p].forget(a.target);
        }
        packs_[p].substituteParallel(local);
    }
    normalizeBottom();
}
void PackedRelationalDomain::assume(const LinearConstraint& c)
{
    if(isBottom()) return;
    auto b=box_; b.assume(c);
    auto reads=vars(c.expression());
    update(reads,reads,[&](auto& d,std::size_t){d.assume(c);});
    box_=std::move(b); normalizeBottom();
}
void PackedRelationalDomain::assume(const TreeConstraint& c)
{
    if(isBottom()) return;
    auto b=box_; b.assume(c);
    auto reads=vars(c.expression());
    update(reads,reads,[&](auto& d,std::size_t){d.assume(c);});
    box_=std::move(b); normalizeBottom();
}
void PackedRelationalDomain::assumeAll(const LinearConstraintSet& cs)
{ for(const auto& c:cs) assume(c); }
void PackedRelationalDomain::forget(Variable v)
{ box_.forget(v); for(auto& p:packs_) p.forget(v); normalizeBottom(); }
void PackedRelationalDomain::project(const std::vector<Variable>& retained)
{ box_.project(retained); for(auto& p:packs_) p.project(retained); normalizeBottom(); }
void PackedRelationalDomain::expand(Variable v,const std::vector<Variable>& copies)
{
    // Expansion duplicates a coordinate's constraints, without asserting that
    // the new coordinates equal the source or one another.
    auto b=box_; b.expand(v,copies);
    std::set<Variable> targets(copies.begin(),copies.end());
    if(targets.empty()) return;
    update(targets,{v},[&](auto& d,std::size_t p){
        std::vector<Variable> local;
        for(auto copy:copies) if(contains(p,copy)) local.push_back(copy);
        d.expand(v,local);
    });
    box_=std::move(b); normalizeBottom();
}
void PackedRelationalDomain::fold(Variable target,const std::vector<Variable>& folded)
{
    box_.fold(target,folded);
    for(std::size_t p=0;p<packs_.size();++p)
    {
        bool inside=contains(p,target); for(auto v:folded) inside&=contains(p,v);
        if(inside) packs_[p].fold(target,folded);
        else
        {
            packs_[p].forget(target); for(auto v:folded) packs_[p].forget(v);
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
    for(std::size_t p=0;p<packs_.size();++p) if(contains(p,v)) b.meetWith(packs_[p].bound(v));
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
    const auto b=box_.supportVariables(); std::set<Variable> s(b.begin(),b.end());
    for(const auto& p:packs_) { auto v=p.supportVariables(); s.insert(v.begin(),v.end()); }
    return {s.begin(),s.end()};
}
LinearConstraintSet PackedRelationalDomain::toConstraints() const
{
    auto cs=box_.toConstraints();
    for(const auto& p:packs_) { auto more=p.toConstraints(); cs.insert(cs.end(),more.begin(),more.end()); }
    return cs;
}
void PackedRelationalDomain::close()
{ box_.close(); for(auto& p:packs_) p.close(); normalizeBottom(); }
void PackedRelationalDomain::canonicalize()
{ box_.canonicalize(); for(auto& p:packs_) p.canonicalize(); normalizeBottom(); }
bool PackedRelationalDomain::hasCompatibleDomain(const AbstractDomain& other) const
{
    return other.isDomain<PackedRelationalDomain>() &&
        *packing_==*static_cast<const PackedRelationalDomain&>(other).packing_;
}
void PackedRelationalDomain::joinDomain(const AbstractDomain& other)
{
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    box_.joinWith(o.box_); for(std::size_t p=0;p<packs_.size();++p) packs_[p].joinWith(o.packs_[p]); normalizeBottom();
}
void PackedRelationalDomain::meetDomain(const AbstractDomain& other)
{
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    box_.meetWith(o.box_); for(std::size_t p=0;p<packs_.size();++p) packs_[p].meetWith(o.packs_[p]); normalizeBottom();
}
void PackedRelationalDomain::widenDomain(const AbstractDomain& other)
{
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    box_.widenWith(o.box_); for(std::size_t p=0;p<packs_.size();++p) packs_[p].widenWith(o.packs_[p]); normalizeBottom();
}
void PackedRelationalDomain::narrowDomain(const AbstractDomain& other)
{
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    box_.narrowWith(o.box_); for(std::size_t p=0;p<packs_.size();++p) packs_[p].narrowWith(o.packs_[p]); normalizeBottom();
}
bool PackedRelationalDomain::isBottomDomain() const
{ if(box_.isBottom()) return true; for(const auto& p:packs_) if(p.isBottom()) return true; return false; }
bool PackedRelationalDomain::isTopDomain() const
{ if(!box_.isTop()) return false; for(const auto& p:packs_) if(!p.isTop()) return false; return true; }
bool PackedRelationalDomain::leqDomain(const AbstractDomain& other) const
{
    if(isBottom()) return true;
    const auto& o=static_cast<const PackedRelationalDomain&>(other);
    if(o.isBottom()) return false;
    // Fast sufficient test on the stored product components.
    bool componentwise = box_.isSubsetOf(o.box_) == CheckResult::True;
    for(std::size_t p=0;componentwise && p<packs_.size();++p)
        componentwise = packs_[p].isSubsetOf(o.packs_[p]) == CheckResult::True;
    if(componentwise) return true;

    // A state can store a unary consequence only in one overlapping pack.
    // Import bounds implied by the entire LHS into temporary LHS components
    // before checking. These consequences preserve its concretization; the
    // original carriers and the RHS are untouched. One reduction pass is a
    // sufficient inclusion check, not a complete relational decision rule.
    for(Variable v:o.box_.supportVariables())
        if(!bound(v).isSubsetOf(o.box_.bound(v))) return false;
    for(std::size_t p=0;p<packs_.size();++p)
    {
        if(packs_[p].isSubsetOf(o.packs_[p])==CheckResult::True) continue;
        auto reduced = packs_[p];
        for(Variable v:packing_->at(p)) restrictBound(reduced,v,bound(v));
        if(reduced.isSubsetOf(o.packs_[p])!=CheckResult::True) return false;
    }
    return true;
}
std::string PackedRelationalDomain::domainToString() const
{
    std::ostringstream s; s<<"PackedOctagon("<<box_.toString();
    for(const auto& p:packs_) s<<","<<p.toString(); s<<")"; return s.str();
}
std::vector<Variable> PackedRelationalDomain::relationalClosureState(const std::vector<Variable>& seeds) const
{
    // Conservative, state-independent envelope for the EXISTING Semi-Sparse
    // reconstruction. It is not the standalone Oh dependency graph.
    std::set<Variable> reached(seeds.begin(),seeds.end()); bool changed=true;
    while(changed)
    {
        changed=false;
        for(const auto& pack:*packing_)
        {
            bool touches=false; for(auto v:pack) touches|=reached.count(v)!=0;
            if(touches) for(auto v:pack) changed|=reached.insert(v).second;
        }
    }
    return {reached.begin(),reached.end()};
}
}
