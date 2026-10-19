//===- LearnedRelationPacking.cpp -- Learned Relation Packing --------===//
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
#include "AE/Core/LearnedRelationPacking.h"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <tuple>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <iomanip>
namespace SVF
{
void PackingFeatureContext::write(std::ostream& out) const
{
    out<<"SVF_PACK_CONTEXT_V1 "<<static_cast<unsigned>(schema)<<' '<<variables.size()<<' '<<relations.size()<<'\n';
    for(const auto& [v,f]:variables)
    {
        std::uint32_t bits=0;
        const bool flags[]= {f.global,f.field,f.arraySize,f.temporary,f.recursive,f.equalsOne,f.globalArraySize,
                             f.libraryResult,f.local,f.localArraySize,f.selfUpdating,f.finiteInterval,f.constantStringSize
                            };
        for(unsigned k=0; k<13; ++k)if(flags[k])bits|=1u<<k;
        for(unsigned k=0; k<5; ++k)if(f.nonOctagonal[k])bits|=1u<<(13+k);
        out<<v.id()<<' '<<static_cast<unsigned>(v.type().kind)<<' '<<bits<<' '<<std::quoted(f.name)<<' '
           <<std::quoted(f.function)<<' '<<f.accessedBy.size();
        for(const auto& fn:f.accessedBy)out<<' '<<std::quoted(fn);
        out<<'\n';
    }
    for(const auto& [edge,f]:relations)
    {
        unsigned bits=0;
        for(unsigned k=0; k<6; ++k)if(f[k])bits|=1u<<k;
        out<<edge.first.id()<<' '<<static_cast<unsigned>(edge.first.type().kind)<<' '
           <<edge.second.id()<<' '<<static_cast<unsigned>(edge.second.type().kind)<<' '<<bits<<'\n';
    }
}
PackingFeatureContext PackingFeatureContext::read(std::istream& input)
{
    std::string magic;
    unsigned schema;
    std::size_t n,e;
    if(!(input>>magic>>schema>>n>>e)||magic!="SVF_PACK_CONTEXT_V1"||(schema!=1&&schema!=2)||n>10000000||e>100000000)
        throw std::invalid_argument("invalid feature context header");
    PackingFeatureContext result;
    result.schema=static_cast<PackingFeatureSchema>(schema);
    const auto variable=[](std::uint32_t id,unsigned kind)
    {
        if(kind>2)throw std::invalid_argument("invalid feature variable type");
        NumericType type;
        type.kind=static_cast<NumericKind>(kind);
        return Variable(id,type);
    };
    for(std::size_t i=0; i<n; ++i)
    {
        std::uint32_t id,bits;
        unsigned kind;
        std::size_t count;
        PackingVariableFacts f;
        if(!(input>>id>>kind>>bits>>std::quoted(f.name)>>std::quoted(f.function)>>count)||bits>=(1u<<18)||count>10000000)
            throw std::invalid_argument("invalid feature facts");
        bool* flags[]= {&f.global,&f.field,&f.arraySize,&f.temporary,&f.recursive,&f.equalsOne,&f.globalArraySize,
                        &f.libraryResult,&f.local,&f.localArraySize,&f.selfUpdating,&f.finiteInterval,&f.constantStringSize
                       };
        for(unsigned k=0; k<13; ++k)*flags[k]=(bits&(1u<<k))!=0;
        for(unsigned k=0; k<5; ++k)f.nonOctagonal[k]=(bits&(1u<<(13+k)))!=0;
        for(std::size_t k=0; k<count; ++k)
        {
            std::string fn;
            if(!(input>>std::quoted(fn)))throw std::invalid_argument("truncated access facts");
            f.accessedBy.insert(fn);
        }
        if(!result.variables.emplace(variable(id,kind),std::move(f)).second)throw std::invalid_argument("duplicate feature variable");
    }
    for(std::size_t i=0; i<e; ++i)
    {
        std::uint32_t a,b;
        unsigned ak,bk,bits;
        if(!(input>>a>>ak>>b>>bk>>bits)||bits>=64)throw std::invalid_argument("invalid feature relation");
        std::array<bool,6> slots{};
        for(unsigned k=0; k<6; ++k)slots[k]=(bits&(1u<<k))!=0;
        if(!result.relations.emplace(RelationEdge{variable(a,ak),variable(b,bk)},slots).second)throw std::invalid_argument("duplicate feature relation");
    }
    std::string extra;
    if(input>>extra)throw std::invalid_argument("trailing feature context data");
    return result;
}
PairFeatures PackingFeatureContext::features(Variable x, Variable y) const
{
    const auto& a=variables.at(x);
    const auto& b=variables.at(y);
    PairFeatures f{};
    const auto pair=std::minmax(x,y);
    if (const auto it=relations.find(pair); it!=relations.end())
        std::copy(it->second.begin(),it->second.end(),f.begin());
    for (unsigned i=0; i<5; ++i) f[6+i]=a.nonOctagonal[i]||b.nonOctagonal[i];
    const bool different=a.function!=b.function;
    f[11]=!a.name.empty() && a.name==b.name && different;
    f[12]=a.global&&b.global;
    f[13]=a.global||b.global;
    f[14]=a.field||b.field;
    f[15]=a.arraySize&&b.arraySize;
    f[16]=a.temporary&&b.temporary;
    f[17]=a.recursive||b.recursive;
    f[18]=a.equalsOne||b.equalsOne;
    f[19]=a.globalArraySize&&b.globalArraySize;
    f[20]=a.libraryResult||b.libraryResult;
    f[21]=a.local&&b.local&&different;
    f[22]=different&&((a.local&&b.localArraySize)||(b.local&&a.localArraySize));
    f[23]=different&&((a.local&&b.temporary)||(b.local&&a.temporary));
    f[24]=(a.global&&b.localArraySize)||(b.global&&a.localArraySize);
    f[25]=(a.temporary&&b.localArraySize)||(b.temporary&&a.localArraySize);
    bool common=false;
    auto ai=a.accessedBy.begin(),bi=b.accessedBy.begin();
    while(ai!=a.accessedBy.end()&&bi!=b.accessedBy.end())
        if(*ai<*bi)++ai;
        else if(*bi<*ai)++bi;
        else
        {
            common=true;
            break;
        }
    f[26]=((a.local&&b.global)||(b.local&&a.global))&&!common;
    f[27]=(a.global&&a.selfUpdating)||(b.global&&b.selfUpdating);
    f[28]=a.finiteInterval||b.finiteInterval;
    f[29]=a.constantStringSize||b.constantStringSize;
    return f;
}
PackingDecisionTree PackingDecisionTree::read(std::istream& input)
{
    std::string magic;
    std::size_t count;
    if (!(input>>magic>>count)||(magic!="SVF_SAS16_TREE_V1"&&magic!="SVF_SAS16_TREE_V2")||count==0||count>100000)
        throw std::invalid_argument("invalid learned model header");
    PackingDecisionTree tree;
    tree.schema_=magic=="SVF_SAS16_TREE_V2"?PackingFeatureSchema::PaperV2:PackingFeatureSchema::LegacyV1;
    for (std::size_t i=0; i<count; ++i)
    {
        Node n;
        if (!(input>>n.feature>>n.left>>n.right>>n.label)) throw std::invalid_argument("truncated learned model");
        if (n.feature < -1 || n.feature>=30 || (n.feature==-1 && (n.label<0||n.label>1||n.left!=-1||n.right!=-1)) ||
                (n.feature>=0 && (n.left<0||n.right<0||n.left>=static_cast<int>(count)||n.right>=static_cast<int>(count))))
            throw std::invalid_argument("invalid learned model node");
        tree.nodes_.push_back(n);
    }
    std::string extra;
    if (input>>extra) throw std::invalid_argument("trailing learned model data");
    // Require an ordinary rooted tree: cycles, shared children and orphans fail.
    std::vector<bool> seen(count);
    std::function<void(int,unsigned)> visit=[&](int i,unsigned depth)
    {
        if (seen[i]||depth>1024) throw std::invalid_argument("cyclic/shared/deep learned tree");
        seen[i]=true;
        if (tree.nodes_[i].feature>=0)
        {
            visit(tree.nodes_[i].left,depth+1);
            visit(tree.nodes_[i].right,depth+1);
        }
    };
    visit(0,0);
    if (std::find(seen.begin(),seen.end(),false)!=seen.end()) throw std::invalid_argument("orphan learned node");
    return tree;
}
bool PackingDecisionTree::predict(const PairFeatures& f) const
{
    if (nodes_.empty()) throw std::invalid_argument("uninitialized learned model");
    std::size_t i=0;
    while (nodes_[i].feature>=0) i=f[nodes_[i].feature]?nodes_[i].right:nodes_[i].left;
    return nodes_[i].label!=0;
}
std::uint32_t PackingDecisionTree::featureMask() const
{
    std::uint32_t mask=0;
    for(const auto& n:nodes_) if(n.feature>=0)mask|=std::uint32_t(1)<<n.feature;
    return mask;
}
namespace
{
struct Facts
{
    enum Bit { Global, Field, ArraySize, Temporary, Recursive, EqualsOne,
               GlobalArraySize, LibraryResult, Local, LocalArraySize, SelfUpdating,
               FiniteInterval, ConstantStringSize, NonOctagonal
             };
    std::uint32_t flags=0,name=0,function=0,access=0;
    bool has(unsigned bit) const
    {
        return (flags&(std::uint32_t(1)<<bit))!=0;
    }
    auto key() const
    {
        return std::tie(flags,name,function,access);
    }
};
bool overlaps(const std::vector<std::uint32_t>& a,const std::vector<std::uint32_t>& b)
{
    auto i=a.begin(),j=b.begin();
    while(i!=a.end()&&j!=b.end())if(*i<*j)++i;
        else if(*j<*i)++j;
        else return true;
    return false;
}
bool feature(unsigned k,const Facts& a,const Facts& b,std::uint8_t relations,
             const std::vector<std::vector<std::uint32_t>>& accessed)
{
    if(k<6) return (relations&(1u<<k))!=0;
    if(k<11) return ((a.flags|b.flags)&(1u<<(Facts::NonOctagonal+k-6)))!=0;
    const bool different=a.function!=b.function;
    switch(k)
    {
    case 11:
        return a.name && a.name==b.name && different;
    case 12:
        return a.has(Facts::Global)&&b.has(Facts::Global);
    case 13:
        return a.has(Facts::Global)||b.has(Facts::Global);
    case 14:
        return a.has(Facts::Field)||b.has(Facts::Field);
    case 15:
        return a.has(Facts::ArraySize)&&b.has(Facts::ArraySize);
    case 16:
        return a.has(Facts::Temporary)&&b.has(Facts::Temporary);
    case 17:
        return a.has(Facts::Recursive)||b.has(Facts::Recursive);
    case 18:
        return a.has(Facts::EqualsOne)||b.has(Facts::EqualsOne);
    case 19:
        return a.has(Facts::GlobalArraySize)&&b.has(Facts::GlobalArraySize);
    case 20:
        return a.has(Facts::LibraryResult)||b.has(Facts::LibraryResult);
    case 21:
        return a.has(Facts::Local)&&b.has(Facts::Local)&&different;
    case 22:
        return different&&((a.has(Facts::Local)&&b.has(Facts::LocalArraySize))||(b.has(Facts::Local)&&a.has(Facts::LocalArraySize)));
    case 23:
        return different&&((a.has(Facts::Local)&&b.has(Facts::Temporary))||(b.has(Facts::Local)&&a.has(Facts::Temporary)));
    case 24:
        return (a.has(Facts::Global)&&b.has(Facts::LocalArraySize))||(b.has(Facts::Global)&&a.has(Facts::LocalArraySize));
    case 25:
        return (a.has(Facts::Temporary)&&b.has(Facts::LocalArraySize))||(b.has(Facts::Temporary)&&a.has(Facts::LocalArraySize));
    case 26:
        return ((a.has(Facts::Local)&&b.has(Facts::Global))||(b.has(Facts::Local)&&a.has(Facts::Global)))&&!overlaps(accessed[a.access],accessed[b.access]);
    case 27:
        return (a.has(Facts::Global)&&a.has(Facts::SelfUpdating))||(b.has(Facts::Global)&&b.has(Facts::SelfUpdating));
    case 28:
        return a.has(Facts::FiniteInterval)||b.has(Facts::FiniteInterval);
    case 29:
        return a.has(Facts::ConstantStringSize)||b.has(Facts::ConstantStringSize);
    default:
        throw std::logic_error("feature out of range");
    }
}
std::uint64_t pairKey(std::size_t a,std::size_t b)
{
    if(a>b)std::swap(a,b);
    return (std::uint64_t(a)<<32)|b;
}
struct PreparedFeatures
{
    std::vector<Facts> facts;
    std::vector<std::vector<std::uint32_t>> accessed;
    std::map<Variable,std::size_t> index;
    std::vector<std::vector<std::size_t>> groups;
    std::vector<std::size_t> groupOf;
};
PreparedFeatures prepare(const PackingFeatureContext& context,const std::vector<Variable>& variables,std::uint32_t used)
{
    std::map<std::string,std::uint32_t> functionIds{{"",0}},nameIds{{"",0}};
    const auto intern=[](auto& ids,const std::string& value)
    {
        auto [it,created]=ids.emplace(value,ids.size());
        (void)created;
        return it->second;
    };
    // A name occurring only in one function can never activate feature12,
    // including with itself. Collapse it without changing any prediction.
    std::map<std::string,std::set<std::string>> nameFunctions;
    if(used&(1u<<11)) for(auto v:variables)
        {
            const auto& f=context.variables.at(v);
            if(!f.name.empty())nameFunctions[f.name].insert(f.function);
        }
    std::map<std::vector<std::uint32_t>,std::uint32_t> accessIds;
    std::vector<std::vector<std::uint32_t>> accessed;
    std::vector<Facts> facts;
    std::map<Variable,std::size_t> index;
    std::uint32_t required=0;
    const auto require=[&](unsigned k,std::initializer_list<unsigned> bits)
    {
        if(used&(1u<<k))for(auto bit:bits)required|=1u<<bit;
    };
    for(unsigned k=6; k<11; ++k)require(k, {Facts::NonOctagonal+k-6});
    require(12, {Facts::Global});
    require(13, {Facts::Global});
    require(14, {Facts::Field});
    require(15, {Facts::ArraySize});
    require(16, {Facts::Temporary});
    require(17, {Facts::Recursive});
    require(18, {Facts::EqualsOne});
    require(19, {Facts::GlobalArraySize});
    require(20, {Facts::LibraryResult});
    require(21, {Facts::Local});
    require(22, {Facts::Local,Facts::LocalArraySize});
    require(23, {Facts::Local,Facts::Temporary});
    require(24, {Facts::Global,Facts::LocalArraySize});
    require(25, {Facts::Temporary,Facts::LocalArraySize});
    require(26, {Facts::Local,Facts::Global});
    require(27, {Facts::Global,Facts::SelfUpdating});
    require(28, {Facts::FiniteInterval});
    require(29, {Facts::ConstantStringSize});
    const bool functionsUsed=(used&((1u<<11)|(1u<<21)|(1u<<22)|(1u<<23)))!=0;
    for(auto v:variables)
    {
        const auto& f=context.variables.at(v);
        Facts c;
        const bool flags[]= {f.global,f.field,f.arraySize,f.temporary,f.recursive,f.equalsOne,
                             f.globalArraySize,f.libraryResult,f.local,f.localArraySize,f.selfUpdating,f.finiteInterval,f.constantStringSize
                            };
        for(unsigned k=0; k<13; ++k)if(flags[k])c.flags|=1u<<k;
        for(unsigned k=0; k<5; ++k)if(f.nonOctagonal[k])c.flags|=1u<<(Facts::NonOctagonal+k);
        c.flags&=required;
        if(functionsUsed)c.function=intern(functionIds,f.function);
        if((used&(1u<<11))&&!f.name.empty()&&nameFunctions.at(f.name).size()>1)c.name=intern(nameIds,f.name);
        std::vector<std::uint32_t> access;
        if((used&(1u<<26))&&(f.local||f.global))for(const auto& fn:f.accessedBy)access.push_back(intern(functionIds,fn));
        std::sort(access.begin(),access.end());
        auto [ai,created]=accessIds.emplace(access,accessed.size());
        if(created)accessed.push_back(access);
        c.access=ai->second;
        index[v]=facts.size();
        facts.push_back(c);
    }
    std::map<std::tuple<std::uint32_t,std::uint32_t,std::uint32_t,std::uint32_t>,std::size_t> signatures;
    std::vector<std::vector<std::size_t>> groups;
    std::vector<std::size_t> groupOf;
    for(std::size_t i=0; i<facts.size(); ++i)
    {
        auto [it,created]=signatures.emplace(facts[i].key(),groups.size());
        if(created)groups.emplace_back();
        groups[it->second].push_back(i);
        groupOf.push_back(it->second);
    }
    return {std::move(facts),std::move(accessed),std::move(index),std::move(groups),std::move(groupOf)};
}
}
ReencodedFeatureHistogram reencodePackingFeatures(const PackingFeatureContext& context,
        std::istream& input,std::ostream* words)
{
    char header[24];
    if(!input.read(header,sizeof(header))||std::string(header,8)!="SVFPAIR1")
        throw std::invalid_argument("invalid archived pair header");
    const auto little=[](const char* bytes,unsigned n)
    {
        std::uint64_t value=0;
        for(unsigned i=0; i<n; ++i)value|=std::uint64_t(static_cast<unsigned char>(bytes[i]))<<(8*i);
        return value;
    };
    const auto n=little(header+8,8),pairs=little(header+16,8);
    if(n!=context.variables.size()||pairs!=n*(n-!context.variables.empty())/2)throw std::invalid_argument("archived/context vocabulary size differs");
    std::vector<Variable> variables;
    for(const auto& entry:context.variables)variables.push_back(entry.first);
    const auto prepared=prepare(context,variables,(1u<<30)-1);
    const auto& groups=prepared.groups;
    const auto& facts=prepared.facts;
    const auto& accessed=prepared.accessed;
    const auto plain=[&](std::size_t a,std::size_t b)
    {
        std::uint32_t bits=0;
        for(unsigned k=6; k<30; ++k)
            if(feature(k,facts[a],facts[b],0,accessed))bits|=1u<<k;
        return bits;
    };
    std::vector<std::uint32_t> table;
    if(groups.size()<=4096)
    {
        table.resize(groups.size()*groups.size());
        for(std::size_t a=0; a<groups.size(); ++a)for(std::size_t b=a; b<groups.size(); ++b)
                table[a*groups.size()+b]=table[b*groups.size()+a]=plain(groups[a].front(),groups[b].front());
    }
    std::unordered_map<std::uint64_t,std::uint32_t> cache;
    std::map<std::uint64_t,std::uint8_t> relations;
    for(const auto& [edge,slots]:context.relations)
    {
        if(edge.second<edge.first)continue;
        auto a=prepared.index.find(edge.first),b=prepared.index.find(edge.second);
        if(a==prepared.index.end()||b==prepared.index.end()||a->second==b->second)continue;
        std::uint8_t mask=0;
        for(unsigned k=0; k<6; ++k)if(slots[k])mask|=1u<<k;
        if(mask)relations[pairKey(a->second,b->second)]|=mask;
    }
    ReencodedFeatureHistogram result;
    result.profiles=groups.size();
    std::unordered_map<std::uint64_t,std::uint64_t> counts;
    constexpr std::size_t blockRecords=65536;
    std::vector<char> buffer(blockRecords*13),encoded(blockRecords*4);
    std::size_t expectedI=0,expectedJ=1;
    auto relation=relations.begin();
    while(result.pairs<pairs)
    {
        const auto count=std::min<std::uint64_t>(blockRecords,pairs-result.pairs);
        if(!input.read(buffer.data(),count*13))throw std::invalid_argument("truncated archived pairs");
        for(std::size_t k=0; k<count; ++k)
        {
            const auto* record=buffer.data()+13*k;
            const auto a=little(record,4),b=little(record+4,4);
            const auto label=static_cast<unsigned char>(record[12]);
            if(a!=expectedI||b!=expectedJ||label>1)throw std::invalid_argument("archived pair order/label differs");
            if(++expectedJ>=n)
            {
                ++expectedI;
                expectedJ=expectedI+1;
            }
            const auto ga=prepared.groupOf[a],gb=prepared.groupOf[b];
            std::uint32_t bits;
            if(!table.empty())bits=table[ga*groups.size()+gb];
            else
            {
                const auto key=pairKey(ga,gb);
                auto hit=cache.find(key);
                if(hit!=cache.end())bits=hit->second;
                else
                {
                    bits=plain(a,b);
                    if(cache.size()<65536)cache.emplace(key,bits);
                }
            }
            const auto key=pairKey(a,b);
            if(relation!=relations.end()&&relation->first==key)
            {
                bits|=relation->second;
                ++relation;
            }
            ++counts[(std::uint64_t(bits)<<1)|label];
            result.positive+=label;
            if(words)for(unsigned byte=0; byte<4; ++byte)encoded[4*k+byte]=static_cast<char>((bits>>(8*byte))&255);
        }
        if(words&&!words->write(encoded.data(),count*4))throw std::runtime_error("feature word output failed");
        result.pairs+=count;
    }
    char extra;
    if(input.get(extra))throw std::invalid_argument("trailing archived pair data");
    for(auto [key,count]:counts)result.histogram[ {static_cast<std::uint32_t>(key>>1),bool(key&1)}]=count;
    return result;
}
LearnedPackingResult LearnedRelationPacking::pack(const std::vector<Variable>& vocabulary) const
{
    const std::set<Variable> distinct(vocabulary.begin(),vocabulary.end());
    const std::vector<Variable> variables(distinct.begin(),distinct.end());
    if(variables.size()>UINT32_MAX) throw std::length_error("learned vocabulary exceeds compact coordinate range");
    const auto used=model_.featureMask();
    const auto prepared=prepare(context_,variables,used);
    const auto& facts=prepared.facts;
    const auto& accessed=prepared.accessed;
    const auto& index=prepared.index;
    const auto& groups=prepared.groups;
    const auto& groupOf=prepared.groupOf;
    LearnedPackingResult result;
    result.pairs=variables.size()*(variables.size()-!variables.empty())/2;
    result.profiles=groups.size();
    const auto predict=[&](std::size_t i,std::size_t j,std::uint8_t relation)
    {
        return model_.predictBy([&](unsigned k)
        {
            return feature(k,facts[i],facts[j],relation,accessed);
        });
    };
    std::map<std::uint64_t,std::uint8_t> exceptions;
    for(const auto& [edge,slots]:context_.relations)
    {
        if(edge.second<edge.first)continue; // Match features()' canonical lookup contract.
        auto a=index.find(edge.first),b=index.find(edge.second);
        if(a==index.end()||b==index.end()||a->second==b->second)continue;
        std::uint8_t mask=0;
        for(unsigned k=0; k<6; ++k)if(slots[k]&&(used&(1u<<k)))mask|=1u<<k;
        if(mask)exceptions[pairKey(a->second,b->second)]|=mask;
    }
    // Bounded memory even when every variable has a distinct profile. This
    // fallback still computes only the features actually visited by the tree.
    // A full symmetric bit table at this bound costs at most 32 MiB.
    // Larger vocabularies still take the bounded-memory lazy fallback.
    if(groups.size()>16384)
    {
        for(std::size_t i=0; i<variables.size(); ++i)
        {
            auto exception=exceptions.lower_bound(pairKey(i,i+1));
            for(std::size_t j=i+1; j<variables.size(); ++j)
            {
                const auto key=pairKey(i,j);
                std::uint8_t relation=0;
                if(exception!=exceptions.end()&&exception->first==key)
                {
                    relation=exception->second;
                    ++exception;
                }
                if(predict(i,j,relation))result.edges.emplace_back(variables[i],variables[j]);
            }
        }
        result.profileEvaluations=result.pairs;
        result.exceptionalPairs=exceptions.size();
    }
    else
    {
        const auto count=groups.size();
        std::vector<bool> baseline(count*count);
        for(std::size_t a=0; a<count; ++a)for(std::size_t b=a; b<count; ++b)
            {
                const bool positive=predict(groups[a].front(),groups[b].front(),0);
                baseline[a*count+b]=baseline[b*count+a]=positive;
                ++result.profileEvaluations;
            }
        std::unordered_set<std::uint64_t> removed;
        for(const auto& [key,relation]:exceptions)
        {
            const std::size_t a=key>>32,b=std::uint32_t(key);
            const bool predicted=predict(a,b,relation),plain=baseline[groupOf[a]*count+groupOf[b]];
            if(plain&&!predicted)removed.insert(key);
            else if(!plain&&predicted)result.edges.emplace_back(variables[a],variables[b]);
        }
        for(std::size_t a=0; a<count; ++a)for(std::size_t b=a; b<count; ++b)
                if(baseline[a*count+b])for(auto i:groups[a])for(auto j:groups[b])
                        {
                            if(a==b&&i>=j)continue;
                            auto x=i,y=j;
                            if(x>y)std::swap(x,y);
                            if(!removed.count(pairKey(x,y)))result.edges.emplace_back(variables[x],variables[y]);
                        }
        std::sort(result.edges.begin(),result.edges.end());
        result.exceptionalPairs=exceptions.size();
    }
    result.packs=connectedComponentPacking(vocabulary,result.edges);
    return result;
}
}
