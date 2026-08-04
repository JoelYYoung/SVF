//===- SVFRelationalBridge.h -- NodeID to relational Core -----*- C++ -*-===//

#ifndef SVF_RELATIONAL_BRIDGE_H
#define SVF_RELATIONAL_BRIDGE_H

#include "AE/Core/RelationalDomain.h"
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
        std::shared_ptr<relational::Manager> manager =
            relational::makeOctagonManager());

    bool tracks(NodeID id) const;
    relational::Variable variable(NodeID id) const;
    const relational::Environment& environment() const;

    void assignConstant(NodeID target, relational::Rational constant);
    void assignAffine(NodeID target, std::vector<AffineTerm> terms,
                      relational::Rational constant = relational::Rational());
    void assumeAffine(std::vector<AffineTerm> terms,
                      relational::Rational constant,
                      relational::ConstraintKind kind);
    void forget(NodeID id);

    void joinWith(const SVFRelationalBridge& other);
    void meetWith(const SVFRelationalBridge& other);
    void widenWith(const SVFRelationalBridge& next,
                   const relational::WideningPolicy& policy = {});
    void narrowWith(const SVFRelationalBridge& next);

    relational::Interval bound(NodeID id) const;
    const relational::AbstractState& state() const { return state_; }
    relational::AbstractState& state() { return state_; }

private:
    relational::LinearExpression
    expression(const std::vector<AffineTerm>& terms,
               relational::Rational constant) const;
    void requireCompatible(const SVFRelationalBridge& other) const;

    std::shared_ptr<relational::Manager> manager_;
    relational::Environment environment_;
    relational::AbstractState state_;
};

} // namespace SVF

#endif // SVF_RELATIONAL_BRIDGE_H
