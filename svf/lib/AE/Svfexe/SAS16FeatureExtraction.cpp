// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AE/Svfexe/SAS16FeatureExtraction.h"
#include "AE/Svfexe/SVFIRAdapter.h"
#include "AE/Svfexe/AllocationSize.h"
#include "AE/Svfexe/FlowInsensitivePackingRanges.h"
#include "MemoryModel/PointerAnalysis.h"
#include "SVFIR/SVFIR.h"
#include <functional>
#include <optional>
namespace SVF
{
namespace AD=AbstractDomain;
AD::PackingFeatureContext extractSAS16Features(SVFIR& ir,const SVFIRAdapter& adapter,
    PointerAnalysis& pts,const std::vector<AD::Variable>& vocabulary,bool legacy)
{
    AD::PackingFeatureContext ctx;
    ctx.schema=legacy?AD::PackingFeatureSchema::LegacyV1:AD::PackingFeatureSchema::PaperV2;
    std::set<AD::Variable> permitted(vocabulary.begin(),vocabulary.end());
    const auto numeric=[&](const SVFVar* v)->std::optional<AD::Variable>
    {
        if (!v) return {};
        const auto* val=SVFUtil::dyn_cast<ValVar>(v);
        if (!val||val->isPointer()||!adapter.contains(*val)) return {};
        const auto x=adapter.variable(*val); return permitted.count(x)?std::optional(x):std::nullopt;
    };
    const auto content=[&](const SVFVar* pointer)
    {
        std::vector<AD::Variable> out;
        if (!pointer||!pointer->isPointer()) return out;
        for (auto id:pts.getPts(pointer->getId()))
            if (const auto* obj=SVFUtil::dyn_cast<ObjVar>(ir.getGNode(id)))
                if (!obj->isPointer()) {const auto x=adapter.contentVariable(*obj);if(permitted.count(x))out.push_back(x);}
        return out;
    };
    // D-005: Sparrow relates allocation, strlen and index features to the
    // Size(a) location; partner Size(o) is added beside the object content.
    const AllocationSizeModel sizeModel(ir);
    const auto sizes=[&](const SVFVar* pointer)
    {
        std::vector<AD::Variable> out;
        if (!pointer||!pointer->isPointer()) return out;
        std::set<const BaseObjVar*> bases;
        for (auto id:pts.getPts(pointer->getId()))
            if (const auto* base=ir.getBaseObject(id)) bases.insert(base);
        for (const auto* base:bases)
            if (sizeModel.find(base)) {const auto x=adapter.sizeVariable(*base);if(permitted.count(x))out.push_back(x);}
        return out;
    };
    std::map<std::string,std::set<std::string>> calls;
    for (auto it=ir.getICFG()->begin();it!=ir.getICFG()->end();++it)
        if (const auto* call=SVFUtil::dyn_cast<CallICFGNode>(it->second))
            if(call->getFun())
            {
                if(call->getCalledFunction())calls[call->getFun()->getName()].insert(call->getCalledFunction()->getName());
                if(!legacy&&pts.hasIndCSCallees(call))for(const auto* callee:pts.getIndCSCallees(call))
                    calls[call->getFun()->getName()].insert(callee->getName());
            }
    std::map<std::string,bool> recursion;
    const auto recursive=[&](const std::string& fn)
    {
        if(auto it=recursion.find(fn);it!=recursion.end())return it->second;
        std::vector<std::string> todo(calls[fn].begin(),calls[fn].end());std::set<std::string> seen;
        while (!todo.empty()) {auto cur=todo.back();todo.pop_back();if(cur==fn)return recursion[fn]=true;
            if(seen.insert(cur).second)todo.insert(todo.end(),calls[cur].begin(),calls[cur].end());}
        return recursion[fn]=false;
    };
    for (auto x:permitted)
    {
        auto& f=ctx.variables[x]; const auto* val=adapter.value(x);const auto* obj=adapter.contentObject(x);
        if(!legacy)if(const auto* sized=adapter.sizeObject(x))
        {
            const auto* base=ir.getBaseObject(sized->getId());
            const auto* allocation=sizeModel.find(base);
            f.name=base->getName()+":size";
            const auto* fn=allocation&&allocation->site&&allocation->site->getICFGNode()
                ?allocation->site->getICFGNode()->getFun():base->getFunction();
            f.function=fn?fn->getName():"";
            f.global=base->isGlobalObj();f.local=!f.function.empty();
            f.arraySize=base->isArray()||base->isHeap();
            f.globalArraySize=f.arraySize&&f.global;
            f.localArraySize=f.arraySize&&!f.global;
            f.constantStringSize=ir.isConstantStringObject(base->getId());
            f.recursive=f.local&&recursive(f.function);
            if(f.local)f.accessedBy.insert(f.function);
            continue;
        }
        const auto* v=val?static_cast<const SVFVar*>(val):static_cast<const SVFVar*>(obj);
        if (!v) continue;
        f.name=v->getName(); const auto* fn=v->getFunction();f.function=fn?fn->getName():"";
        f.global=obj&&ir.getBaseObject(obj->getId())->isGlobalObj();
        f.field=obj&&SVFUtil::isa<GepObjVar>(obj)&&ir.getBaseObject(obj->getId())->isStruct();
        f.local=!f.function.empty();f.temporary=val&&!SVFUtil::isa<ArgValVar>(val)&&!SVFUtil::isa<RetValPN>(val);
        f.recursive=f.local&&recursive(f.function);if(f.local)f.accessedBy.insert(f.function);
    }
    const auto relation=[&](std::optional<AD::Variable> a,std::optional<AD::Variable> b,unsigned index)
    {if(a&&b&&*a!=*b)ctx.relations[std::minmax(*a,*b)][index]=true;};
    const auto mark=[&](const SVFVar* v,unsigned index)
    {if(const auto x=numeric(v))ctx.variables[*x].nonOctagonal[index]=true;};
    // A conservative finite-range subset: every definition must have finite
    // constant/affine/phi inputs; cycles, calls, arguments and memory are unknown.
    std::map<AD::Variable,std::vector<std::vector<const SVFVar*>>> definitions;
    std::set<AD::Variable> unknown;
    std::vector<const SVFStmt*> statements;
    for(auto kind:{SVFStmt::BinaryOp,SVFStmt::Copy,SVFStmt::Cmp,SVFStmt::Load,SVFStmt::Store,SVFStmt::Phi,SVFStmt::Select,SVFStmt::UnaryOp})
        for(const auto* s:ir.getSVFStmtSet(kind))statements.push_back(s);
    std::map<const SVFVar*,std::vector<AD::Variable>> loadedContents;
    std::map<const SVFVar*,std::vector<AD::Variable>> updateSources;
    for(const auto* s:statements)if(const auto* load=SVFUtil::dyn_cast<LoadStmt>(s))
        loadedContents[load->getLHSVar()]=content(load->getRHSVar());
    for(const auto* s:statements)if(const auto* binary=SVFUtil::dyn_cast<BinaryOPStmt>(s))
        for(auto operand:binary->getOpndVars())
        {
            const auto& objects=loadedContents[operand];
            auto& sources=updateSources[binary->getRes()];sources.insert(sources.end(),objects.begin(),objects.end());
        }
    for(const auto* s:statements)
    {
        const auto use=[&](const SVFVar* v)
        {
            if(const auto x=numeric(v))if(s->getICFGNode()&&s->getICFGNode()->getFun())ctx.variables[*x].accessedBy.insert(s->getICFGNode()->getFun()->getName());
        };
        if(const auto* b=SVFUtil::dyn_cast<BinaryOPStmt>(s))
        {
            const auto* a=b->getOpVar(0);const auto* c=b->getOpVar(1);use(a);use(c);use(b->getRes());
            const auto* ka=SVFUtil::dyn_cast<ConstIntValVar>(a);const auto* kc=SVFUtil::dyn_cast<ConstIntValVar>(c);
            const bool affine=(b->getOpcode()==BinaryOPStmt::Add&&(ka||kc))||(b->getOpcode()==BinaryOPStmt::Sub&&kc);
            if(affine){relation(numeric(b->getRes()),numeric(a),0);relation(numeric(b->getRes()),numeric(c),0);}
            if(const auto x=numeric(b->getRes())) {if(affine)definitions[*x].push_back({a,c});else unknown.insert(*x);}
            if(b->getOpcode()==BinaryOPStmt::Mul)
            {
                if(ka||kc) {const auto* k=ka?ka:kc;if(k->getSExtValue()!=1){mark(ka?c:a,0);}}
                else {mark(a,1);mark(c,1);}
            }
            if((b->getOpcode()==BinaryOPStmt::SDiv||b->getOpcode()==BinaryOPStmt::UDiv)&&!kc){mark(a,2);mark(c,2);}
            if(b->getOpcode()>=BinaryOPStmt::Shl&&b->getOpcode()<=BinaryOPStmt::Xor){mark(a,3);mark(c,3);}
            if(!affine)mark(b->getRes(),4);
        }
        else if(const auto* c=SVFUtil::dyn_cast<CmpStmt>(s))
        {
            const auto* a=c->getOpVar(0);const auto* b=c->getOpVar(1);use(a);use(b);
            if(c->getPredicate()!=CmpStmt::ICMP_NE)relation(numeric(a),numeric(b),1);
            if(c->getPredicate()==CmpStmt::ICMP_EQ)
            {
                for(auto pair:{std::make_pair(a,b),std::make_pair(b,a)})
                    if(const auto* k=SVFUtil::dyn_cast<ConstIntValVar>(pair.second))
                        if(k->getSExtValue()==1||k->getSExtValue()==-1)
                            if(const auto x=numeric(pair.first))ctx.variables[*x].equalsOne=true;
            }
            if(const auto x=numeric(c->getRes()))definitions[*x].push_back({});
        }
        else if(const auto* copy=SVFUtil::dyn_cast<CopyStmt>(s))
        {
            use(copy->getLHSVar());use(copy->getRHSVar());relation(numeric(copy->getLHSVar()),numeric(copy->getRHSVar()),0);
            if(const auto x=numeric(copy->getLHSVar()))definitions[*x].push_back({copy->getRHSVar()});
        }
        else if(const auto* load=SVFUtil::dyn_cast<LoadStmt>(s))
        {
            use(load->getLHSVar());if(const auto x=numeric(load->getLHSVar()))unknown.insert(*x);
            for(auto x:content(load->getRHSVar()))if(s->getICFGNode()->getFun())ctx.variables[x].accessedBy.insert(s->getICFGNode()->getFun()->getName());
        }
        else if(const auto* store=SVFUtil::dyn_cast<StoreStmt>(s))
        {
            use(store->getRHSVar());for(auto x:content(store->getLHSVar()))
            {
                if(s->getICFGNode()->getFun())ctx.variables[x].accessedBy.insert(s->getICFGNode()->getFun()->getName());
                // Identify an update sourced from a load of this same content.
                const auto& sources=updateSources[store->getRHSVar()];
                if(std::find(sources.begin(),sources.end(),x)!=sources.end())ctx.variables[x].selfUpdating=true;
            }
        }
        else if(const auto* multi=SVFUtil::dyn_cast<MultiOpndStmt>(s))
        {if(const auto x=numeric(multi->getRes()))definitions[*x].emplace_back(multi->getOpndVars().begin(),multi->getOpndVars().end());}
        else if(const auto* unary=SVFUtil::dyn_cast<UnaryOPStmt>(s))
            if(const auto x=numeric(unary->getRes()))unknown.insert(*x);
    }
    std::map<AD::Variable,int> finite;
    std::function<bool(AD::Variable)> bounded=[&](AD::Variable x)
    {
        if(finite.count(x))return finite[x]==2;
        finite[x]=1;if(unknown.count(x)||!definitions.count(x))return false;
        for(const auto& operands:definitions[x])for(const auto* v:operands)
            if(!SVFUtil::isa<ConstIntValVar>(v)) {const auto y=numeric(v);if(!y||!bounded(*y))return false;}
        finite[x]=2;return true;
    };
    if(legacy)for(auto x:permitted)ctx.variables[x].finiteInterval=bounded(x);
    else
    {
        const auto ranges=flowInsensitivePackingRanges(ir,adapter,pts,vocabulary,sizeModel);
        for(auto x:permitted){const auto& range=ranges.at(x);ctx.variables[x].finiteInterval=
            !range.isBottom()&&range.lower().isFinite()&&range.upper().isFinite();}
    }
    std::map<AD::Variable,std::set<AD::Variable>> strlenSources;
    for(auto it=ir.getICFG()->begin();it!=ir.getICFG()->end();++it)
    {
        const auto* call=SVFUtil::dyn_cast<CallICFGNode>(it->second);
        if(!call||!call->getCalledFunction())continue;
        const auto name=call->getCalledFunction()->getName();const auto target=numeric(call->getRetICFGNode()->getActualRet());
        if(target&&call->getCalledFunction()->isDeclaration())ctx.variables[*target].libraryResult=true;
        if((name=="malloc"||name=="calloc"||name=="realloc")&&call->arg_size())
        {
            const auto size=numeric(call->getArgument(name=="realloc"?1:0));
            if(size){ctx.variables[*size].arraySize=true;ctx.variables[*size].localArraySize=true;}
            for(auto obj:content(call->getRetICFGNode()->getActualRet()))relation(obj,size,2);
            for(auto obj:sizes(call->getRetICFGNode()->getActualRet()))relation(obj,size,2);
        }
        if(name=="strlen"&&call->arg_size()&&target)
            for(auto obj:content(call->getArgument(0))) {relation(target,obj,3);strlenSources[*target].insert(obj);}
        if(name=="strlen"&&call->arg_size()&&target)
            for(auto obj:sizes(call->getArgument(0))) {relation(target,obj,3);strlenSources[*target].insert(obj);}
    }
    // Copy closure for indirect strlen assignment (feature 5).
    std::set<AD::Variable> indirectStrlen;
    bool changed=true;
    while(changed){changed=false;for(const auto* s:statements)if(const auto* copy=SVFUtil::dyn_cast<CopyStmt>(s))
        {auto to=numeric(copy->getLHSVar()),from=numeric(copy->getRHSVar());if(to&&from)
            for(auto source:strlenSources[*from])if(strlenSources[*to].insert(source).second){changed=true;indirectStrlen.insert(*to);}}}
    for(const auto& [x,sources]:strlenSources)if(indirectStrlen.count(x))for(auto y:sources)relation(x,y,4);
    for(const auto* s:ir.getSVFStmtSet(SVFStmt::Gep))if(const auto* gep=SVFUtil::dyn_cast<GepStmt>(s))
        for(const auto& pair:gep->getOffsetVarAndGepTypePairVec())
        {
            for(auto obj:content(gep->getRHSVar()))relation(obj,numeric(pair.first),5);
            for(auto obj:sizes(gep->getRHSVar()))relation(obj,numeric(pair.first),5);
        }
    return ctx;
}
}
