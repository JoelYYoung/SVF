// SPDX-License-Identifier: AGPL-3.0-or-later
#ifndef SVF_AE_LEARNED_RELATION_PACKING_H
#define SVF_AE_LEARNED_RELATION_PACKING_H
#include "AE/Core/RelationPacking.h"
#include <array>
#include <istream>
#include <map>
#include <set>
#include <string>
#include <stdexcept>
#include <cstdint>
namespace SVF::AbstractDomain
{
using PairFeatures = std::array<bool, 30>;
enum class PackingFeatureSchema { LegacyV1=1, PaperV2=2 };
struct PackingVariableFacts
{
    std::string name, function;
    std::set<std::string> accessedBy;
    std::array<bool,5> nonOctagonal{}; // f7..f11
    bool global=false, field=false, arraySize=false, temporary=false;
    bool recursive=false, equalsOne=false, globalArraySize=false, libraryResult=false;
    bool local=false, localArraySize=false, selfUpdating=false;
    bool finiteInterval=false, constantStringSize=false;
};
struct PackingFeatureContext
{
    PackingFeatureSchema schema=PackingFeatureSchema::LegacyV1;
    std::map<Variable,PackingVariableFacts> variables;
    std::map<RelationEdge,std::array<bool,6>> relations;
    PairFeatures features(Variable x, Variable y) const;
    void write(std::ostream&) const;
    static PackingFeatureContext read(std::istream&);
};
class PackingDecisionTree
{
public:
    struct Node { int feature, left, right, label; };
    static PackingDecisionTree read(std::istream& input);
    bool predict(const PairFeatures& features) const;
    template<class Evaluate> bool predictBy(Evaluate&& evaluate) const
    {
        if(nodes_.empty()) throw std::invalid_argument("uninitialized learned model");
        std::size_t i=0;
        while(nodes_[i].feature>=0) i=evaluate(static_cast<unsigned>(nodes_[i].feature))?nodes_[i].right:nodes_[i].left;
        return nodes_[i].label!=0;
    }
    std::uint32_t featureMask() const;
    PackingFeatureSchema schema() const { return schema_; }
private:
    PackingFeatureSchema schema_=PackingFeatureSchema::LegacyV1;
    std::vector<Node> nodes_;
};
struct LearnedPackingResult
{
    RelationPacking packs;
    std::vector<RelationEdge> edges;
    std::size_t pairs=0;
    std::size_t profiles=0, profileEvaluations=0, exceptionalPairs=0;
};
struct ReencodedFeatureHistogram
{
    std::uint64_t pairs=0,positive=0;
    std::size_t profiles=0;
    std::map<std::pair<std::uint32_t,bool>,std::uint64_t> histogram;
};
// Reexpress new features on individually archived labels. Never changes the
// labels, runs impact, samples pairs or accepts a different variable count.
ReencodedFeatureHistogram reencodePackingFeatures(const PackingFeatureContext&,
    std::istream& originalPairs,std::ostream* featureWords=nullptr);
/// Context and model are bound before pack(V); no impact or numerical AE access.
class LearnedRelationPacking
{
public:
    LearnedRelationPacking(PackingFeatureContext context, PackingDecisionTree model)
        : context_(std::move(context)), model_(std::move(model))
    {
        if(context_.schema!=model_.schema()) throw std::invalid_argument("learned model/feature schema mismatch");
    }
    LearnedPackingResult pack(const std::vector<Variable>& vocabulary) const;
private:
    PackingFeatureContext context_;
    PackingDecisionTree model_;
};
}
#endif
