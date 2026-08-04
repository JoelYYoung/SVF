//===- OctagonDomain.h -- Private relational backend interface -*- C++ -*-===//

#ifndef RELATIONAL_OCTAGON_DOMAIN_H
#define RELATIONAL_OCTAGON_DOMAIN_H

#include "AE/Core/RelationalDomain.h"

#include <memory>

namespace relational
{

class BackendState
{
public:
    virtual ~BackendState() = default;
    virtual std::unique_ptr<BackendState> clone() const = 0;
};

class DomainBackend
{
public:
    virtual ~DomainBackend() = default;

    virtual const char* name() const = 0;
    virtual DomainCapabilities capabilities() const = 0;
    virtual std::unique_ptr<BackendState>
    top(const Environment& environment) const = 0;
    virtual std::unique_ptr<BackendState>
    bottom(const Environment& environment) const = 0;

    virtual ApproximationKind assign(BackendState& state,
                                     const Environment& environment,
                                     Variable target,
                                     const LinearExpression& expression) const = 0;
    virtual ApproximationKind assume(BackendState& state,
                                     const Environment& environment,
                                     const LinearConstraint& constraint) const = 0;
    virtual void forget(BackendState& state, const Environment& environment,
                        Variable variable) const = 0;

    virtual std::unique_ptr<BackendState>
    join(const BackendState& lhs, const BackendState& rhs) const = 0;
    virtual std::unique_ptr<BackendState>
    meet(const BackendState& lhs, const BackendState& rhs) const = 0;
    virtual std::unique_ptr<BackendState>
    widen(const BackendState& current, const BackendState& next,
          const WideningPolicy& policy) const = 0;
    virtual std::unique_ptr<BackendState>
    narrow(const BackendState& current, const BackendState& next) const = 0;
    virtual std::unique_ptr<BackendState>
    projectLowerBounds(const BackendState& state) const = 0;
    virtual std::unique_ptr<BackendState>
    changeEnvironment(const BackendState& state,
                      const Environment& oldEnvironment,
                      const Environment& newEnvironment,
                      bool projectNewVariables) const = 0;

    virtual bool isBottom(const BackendState& state) const = 0;
    virtual bool isTop(const BackendState& state) const = 0;
    virtual bool leq(const BackendState& lhs,
                     const BackendState& rhs) const = 0;
    virtual Interval bound(const BackendState& state,
                           const Environment& environment,
                           Variable variable) const = 0;
    virtual ConstraintSet
    constraints(const BackendState& state,
                const Environment& environment) const = 0;
    virtual std::string toString(const BackendState& state,
                                 const Environment& environment) const = 0;
};

std::shared_ptr<DomainBackend>
makeOctagonBackend(const OctagonOptions& options);

} // namespace relational

#endif // RELATIONAL_OCTAGON_DOMAIN_H
