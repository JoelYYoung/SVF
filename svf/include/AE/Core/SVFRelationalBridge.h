//===- SVFRelationalBridge.h -- NodeID to relational Core -----*- C++ -*-===//

#ifndef SVF_RELATIONAL_BRIDGE_H
#define SVF_RELATIONAL_BRIDGE_H

#include "AE/Core/IntervalValue.h"
#include "AE/Core/OctagonDomain.h"
#include "Util/GeneralType.h"

#include <string>
#include <utility>
#include <vector>

namespace SVF
{

struct TrackedRelationalVariable
{
    NodeID id;
    relational::NumericType type;
    std::string name;
};

/// Thin integration boundary used by AE's existing updateStateOnXXX methods.
/// It intentionally does not inspect or dispatch SVFStmt objects: the AE
/// transfer layer decides whether an operation is affine and then calls this
/// class with the corresponding coefficients.
class SVFRelationalBridge
{
public:
    using AffineTerm = std::pair<NodeID, relational::Rational>;

    explicit SVFRelationalBridge(
        std::vector<TrackedRelationalVariable> variables,
        std::shared_ptr<relational::AbstractDomain> domain =
            relational::makeOctagonDomain());

    bool tracks(NodeID id) const;
    relational::Variable variable(NodeID id) const;
    const relational::Environment& environment() const;
    void changeTrackedVariables(
        std::vector<TrackedRelationalVariable> variables,
        bool projectNewVariables = false);

    void assignConstant(NodeID target, relational::Rational constant);
    void assignAffine(NodeID target, std::vector<AffineTerm> terms,
                      relational::Rational constant = relational::Rational());
    void assumeAffine(std::vector<AffineTerm> terms,
                      relational::Rational constant,
                      relational::ConstraintKind kind);
    void assignInterval(NodeID target, const IntervalValue& interval);
    void meetInterval(NodeID target, const IntervalValue& interval);
    void forget(NodeID id);

    void joinWith(const SVFRelationalBridge& other);
    void meetWith(const SVFRelationalBridge& other);
    void widenWith(const SVFRelationalBridge& next,
                   const relational::WideningPolicy& policy = {});
    void narrowWith(const SVFRelationalBridge& next);
    bool equals(const SVFRelationalBridge& other) const;
    bool includedIn(const SVFRelationalBridge& other) const;

    relational::Interval bound(NodeID id) const;
    const relational::AbstractState& state() const
    {
        return state_;
    }
    relational::AbstractState& state()
    {
        return state_;
    }

private:
    relational::LinearExpression expression(
        const std::vector<AffineTerm>& terms,
        relational::Rational constant) const;
    void requireCompatible(const SVFRelationalBridge& other) const;
    void constrainInterval(NodeID target, const IntervalValue& interval);

    std::shared_ptr<relational::AbstractDomain> domain_;
    relational::Environment environment_;
    relational::AbstractState state_;
};

} // namespace SVF

#endif // SVF_RELATIONAL_BRIDGE_H
