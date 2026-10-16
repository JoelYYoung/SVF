#include "AE/Svfexe/FlowInsensitivePackingRanges.h"
#include "AE/Svfexe/SVFIRAdapter.h"
#include "AE/Svfexe/AllocationSize.h"
#include "SVFIR/SVFIR.h"
#include "MemoryModel/PointerAnalysis.h"
#include <deque>
#include <functional>
#include <optional>
#include <set>
namespace SVF
{
namespace A=AbstractDomain;
std::map<A::Variable,A::Interval> flowInsensitivePackingRanges(SVFIR& ir,const SVFIRAdapter& adapter,
    PointerAnalysis& pts,const std::vector<A::Variable>& vocabulary,const AllocationSizeModel& sizes)
{
    using V=A::Variable;using I=A::Interval;
    std::set<V> allowed(vocabulary.begin(),vocabulary.end());
    std::map<V,I> values;for(auto v:allowed)values.emplace(v,I::bottom());
    const auto scalar=[&](const SVFVar* v)->std::optional<V>
    {
        const auto* x=v?SVFUtil::dyn_cast<ValVar>(v):nullptr;
        if(!x||x->isPointer()||!adapter.contains(*x))return {};
        const auto variable=adapter.variable(*x);return allowed.count(variable)?std::optional(variable):std::nullopt;
    };
    const auto read=[&](const SVFVar* v)
    {
        if(const auto* c=v?SVFUtil::dyn_cast<ConstIntValVar>(v):nullptr)return I::singleton(A::Rational(c->getSExtValue()));
        const auto x=scalar(v);return x?values.at(*x):I::top();
    };
    const auto objects=[&](const SVFVar* p)
    {
        std::vector<V> result;if(!p||!p->isPointer())return result;
        for(auto id:pts.getPts(p->getId()))if(const auto* o=SVFUtil::dyn_cast<ObjVar>(ir.getGNode(id)))
            if(!o->isPointer()){const auto x=adapter.contentVariable(*o);if(allowed.count(x))result.push_back(x);}
        return result;
    };
    struct Rule{V target;std::vector<V> reads;std::function<I()> evaluate;};
    std::vector<Rule> rules;std::set<V> defined;
    const auto add=[&](V target,std::vector<V> reads,std::function<I()> evaluate)
    {if(allowed.count(target)){defined.insert(target);rules.push_back({target,std::move(reads),std::move(evaluate)});}};
    const auto uses=[&](const std::vector<const SVFVar*>& operands)
    {std::vector<V> out;for(auto v:operands)if(auto x=scalar(v))out.push_back(*x);return out;};
    for(auto kind:{SVFStmt::BinaryOp,SVFStmt::Copy,SVFStmt::Cmp,SVFStmt::Load,SVFStmt::Store,
                   SVFStmt::Phi,SVFStmt::Select,SVFStmt::UnaryOp,SVFStmt::Call,SVFStmt::Ret})
        for(const auto* statement:ir.getSVFStmtSet(kind))
        {
            if(const auto* b=SVFUtil::dyn_cast<BinaryOPStmt>(statement))
            {
                const auto x=scalar(b->getRes());if(!x)continue;
                const auto* l=b->getOpVar(0);const auto* r=b->getOpVar(1);const auto opcode=b->getOpcode();
                add(*x,uses({l,r}),[&,l,r,opcode]{const auto a=read(l),b=read(r);if(a.isBottom()||b.isBottom())return I::bottom();
                    switch(opcode){case BinaryOPStmt::Add:return A::add(a,b);case BinaryOPStmt::Sub:return A::subtract(a,b);
                    case BinaryOPStmt::Mul:return A::multiply(a,b);case BinaryOPStmt::SDiv:case BinaryOPStmt::UDiv:return A::divide(a,b);
                    case BinaryOPStmt::SRem:case BinaryOPStmt::URem:return A::remainder(a,b);
                    case BinaryOPStmt::And:return A::bitwiseAnd(a,b);case BinaryOPStmt::Or:return A::bitwiseOr(a,b);
                    case BinaryOPStmt::Xor:return A::bitwiseXor(a,b);case BinaryOPStmt::Shl:return A::shiftLeft(a,b);
                    case BinaryOPStmt::LShr:case BinaryOPStmt::AShr:return A::shiftRight(a,b);default:return I::top();}});
            }
            else if(const auto* c=SVFUtil::dyn_cast<CmpStmt>(statement))
            {if(auto x=scalar(c->getRes()))add(*x,{},[]{return I::closed(A::Rational(0),A::Rational(1));});}
            else if(const auto* copy=SVFUtil::dyn_cast<CopyStmt>(statement))
            {
                const auto x=scalar(copy->getLHSVar());if(!x)continue;const auto* rhs=copy->getRHSVar();const auto* lhs=copy->getLHSVar();const auto cast=copy->getCopyKind();
                add(*x,uses({rhs}),[&,rhs,lhs,cast]{if(cast==CopyStmt::COPYVAL||cast==CopyStmt::SEXT)return read(rhs);
                    if(cast==CopyStmt::ZEXT||cast==CopyStmt::TRUNC){const auto* t=SVFUtil::dyn_cast<SVFIntegerType>((cast==CopyStmt::TRUNC?lhs:rhs)->getType());
                        return t?A::wrapIntegerInterval(read(rhs),t->getByteSize()*8,cast!=CopyStmt::ZEXT):I::top();}return I::top();});
            }
            else if(const auto* ret=SVFUtil::dyn_cast<RetPE>(statement))
            {if(auto x=scalar(ret->getLHSVar())){const auto* rhs=ret->getRHSVar();add(*x,uses({rhs}),[&,rhs]{return read(rhs);});}}
            else if(const auto* load=SVFUtil::dyn_cast<LoadStmt>(statement))
            {if(auto x=scalar(load->getLHSVar())){auto sources=objects(load->getRHSVar());
                bool missing=sources.size()!=pts.getPts(load->getRHSVar()->getId()).count();
                for(auto id:pts.getPts(load->getRHSVar()->getId()))if(const auto* base=ir.getBaseObject(id))missing|=base->isBlackHoleObj();
                add(*x,sources,[&,sources,missing]{
                if(missing||sources.empty())return I::top();auto result=I::bottom();for(auto v:sources)result.joinWith(values.at(v));return result;});}}
            else if(const auto* store=SVFUtil::dyn_cast<StoreStmt>(statement))
            {const auto* rhs=store->getRHSVar();for(auto x:objects(store->getLHSVar()))add(x,uses({rhs}),[&,rhs]{return read(rhs);});}
            else if(const auto* multiple=SVFUtil::dyn_cast<MultiOpndStmt>(statement))
            {if(auto x=scalar(multiple->getRes())){std::vector<const SVFVar*> operands(multiple->getOpndVars().begin(),multiple->getOpndVars().end());
                add(*x,uses(operands),[&,operands]{auto result=I::bottom();for(auto v:operands)result.joinWith(read(v));return operands.empty()?I::top():result;});}}
            else if(const auto* unary=SVFUtil::dyn_cast<UnaryOPStmt>(statement))
            {if(auto x=scalar(unary->getRes()))add(*x,{},[]{return I::top();});}
        }
    for(const auto& size:sizes.all())
    {
        const auto v=adapter.sizeVariable(*size.object);if(!allowed.count(v))continue;
        if(size.known&&size.operand){const auto* rhs=size.operand;add(v,uses({rhs}),[&,rhs]{return read(rhs);});}
        else if(size.known){const auto constant=size.constant;add(v,{},[constant]{return I::singleton(A::Rational(constant));});}
        else add(v,{},[]{return I::top();});
    }
    for(auto it=ir.getICFG()->begin();it!=ir.getICFG()->end();++it)
        if(const auto* call=SVFUtil::dyn_cast<CallICFGNode>(it->second))
            if(!call->getCalledFunction()||call->getCalledFunction()->isDeclaration())
            {
                if(auto x=scalar(call->getRetICFGNode()->getActualRet()))add(*x,{},[]{return I::top();});
                for(unsigned i=0;i<call->arg_size();++i)for(auto x:objects(call->getArgument(i)))add(x,{},[]{return I::top();});
            }
    // Undefined inputs and memory with no known writer stay unknown. This is
    // a feature pre-analysis; it must never make missing information finite.
    for(auto& [v,value]:values)if(!defined.count(v))value=I::top();
    std::map<V,std::vector<std::size_t>> readers;std::map<V,std::set<V>> outgoing;std::map<V,std::size_t> degree;
    for(auto v:allowed)degree[v]=0;
    for(std::size_t i=0;i<rules.size();++i)for(auto v:rules[i].reads)
    {readers[v].push_back(i);if(outgoing[v].insert(rules[i].target).second)++degree[rules[i].target];}
    std::deque<V> acyclic;for(auto [v,n]:degree)if(!n)acyclic.push_back(v);
    while(!acyclic.empty()){const auto v=acyclic.front();acyclic.pop_front();for(auto target:outgoing[v])if(--degree[target]==0)acyclic.push_back(target);}
    std::deque<std::size_t> work;std::vector<bool> queued(rules.size(),true);for(std::size_t i=0;i<rules.size();++i)work.push_back(i);
    std::map<V,unsigned> changes;
    while(!work.empty())
    {
        const auto i=work.front();work.pop_front();queued[i]=false;const auto& rule=rules[i];
        auto next=values.at(rule.target);next.joinWith(rule.evaluate());
        if(next==values.at(rule.target))continue;
        if(degree[rule.target]&&++changes[rule.target]>3){auto widened=values.at(rule.target);widened.widenWith(next);next=std::move(widened);}
        values[rule.target]=std::move(next);
        for(auto r:readers[rule.target])if(!queued[r]){work.push_back(r);queued[r]=true;}
    }
    return values;
}
}
