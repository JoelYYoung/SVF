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
    SVF::NumericType type;
    std::string name;
};

/// Thin integration boundary used by AE's existing updateStateOnXXX methods.
/// It intentionally does not inspect or dispatch SVFStmt objects: the AE
/// transfer layer decides whether an operation is affine and then calls this
/// class with the corresponding coefficients.
class SVFRelationalBridge
{
public:
    using AffineTerm = std::pair<NodeID, SVF::Rational>;

    explicit SVFRelationalBridge(
        std::vector<TrackedRelationalVariable> variables,
        SVF::OctagonConfig config = {});

    bool tracks(NodeID id) const;
    SVF::Variable variable(NodeID id) const;
    const SVF::Environment& environment() const;
    void changeTrackedVariables(
        std::vector<TrackedRelationalVariable> variables,
        bool projectNewVariables = false);
    void setTop();
    void setBottom();

    void assignConstant(NodeID target, SVF::Rational constant);
    void assignAffine(NodeID target, std::vector<AffineTerm> terms,
                      SVF::Rational constant = SVF::Rational());
    void assumeAffine(std::vector<AffineTerm> terms,
                      SVF::Rational constant,
                      SVF::ConstraintKind kind);
    void assignInterval(NodeID target, const IntervalValue& interval);
    void meetInterval(NodeID target, const IntervalValue& interval);
    void forget(NodeID id);

    void joinWith(const SVFRelationalBridge& other);
    void meetWith(const SVFRelationalBridge& other);
    void widenWith(const SVFRelationalBridge& next,
                   const SVF::WideningPolicy& policy = {});
    void narrowWith(const SVFRelationalBridge& next);
    bool equals(const SVFRelationalBridge& other) const;
    bool includedIn(const SVFRelationalBridge& other) const;
    bool isBottom() const;

    SVF::Interval bound(NodeID id) const;
    /// Project an integer Octagon bound into AE's signed 64-bit interval
    /// representation. Unsupported numeric kinds or unrepresentable finite
    /// endpoints conservatively project to top.
    IntervalValue projectInterval(NodeID id) const;
    const SVF::OctagonState& state() const
    {
        return state_;
    }
    SVF::OctagonState& state()
    {
        return state_;
    }

private:
    SVF::LinearExpression expression(
        const std::vector<AffineTerm>& terms,
        SVF::Rational constant) const;
    void requireCompatible(const SVFRelationalBridge& other) const;
    void constrainInterval(NodeID target, const IntervalValue& interval);

    SVF::Environment environment_;
    SVF::OctagonState state_;
};

} // namespace SVF

#endif // SVF_RELATIONAL_BRIDGE_H
