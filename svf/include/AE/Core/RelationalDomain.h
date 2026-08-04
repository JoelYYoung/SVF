//===- RelationalDomain.h -- APRON-style relational API --------*- C++ -*-===//

#ifndef RELATIONAL_DOMAIN_H
#define RELATIONAL_DOMAIN_H

#include "AE/Core/RelationalExpression.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace relational
{

class BackendState;
class DomainBackend;
class AbstractState;

enum class CheckResult
{
    False,
    True,
    Unknown
};

const char* toString(CheckResult result);

enum class ApproximationKind
{
    Exact,
    BestAbstraction,
    SoundOverApproximation,
    UnsupportedFallback
};

enum class OperationKind
{
    Assignment,
    Assumption,
    Join,
    Meet,
    Widening,
    Narrowing,
    Conversion
};

struct Diagnostic
{
    OperationKind operation;
    ApproximationKind approximation;
    std::string reason;
};

class DiagnosticSink
{
public:
    virtual ~DiagnosticSink() = default;
    virtual void report(const Diagnostic& diagnostic) = 0;
};

struct DomainCapabilities
{
    bool strictInequalities = false;
    bool integerTightening = false;
    bool thresholdWidening = false;
    bool narrowing = false;
    bool treeExpressions = false;
};

struct OctagonOptions
{
    bool strongClosure = true;
    bool integerTightening = true;
    std::shared_ptr<DiagnosticSink> diagnostics;
};

struct WideningPolicy
{
    /// Constants for normalized +/-x +/-y <= c and +/-x <= c templates.
    std::vector<Rational> thresholds;
};

struct Box
{
    std::map<Variable, Interval> bounds;
};

enum class ConversionQuality
{
    Exact,
    BestAbstraction,
    SoundButLossy
};

class Manager final : public std::enable_shared_from_this<Manager>
{
public:
    ~Manager();

    const char* backendName() const;
    DomainCapabilities capabilities() const;

    AbstractState top(const Environment& environment) const;
    AbstractState bottom(const Environment& environment) const;
    AbstractState fromBox(const Environment& environment, const Box& box) const;
    AbstractState fromConstraints(const Environment& environment,
                                  const ConstraintSet& constraints) const;

private:
    friend class AbstractState;
    friend std::shared_ptr<Manager>
    makeOctagonManager(const OctagonOptions& options);

    Manager(std::shared_ptr<DomainBackend> backend,
            std::shared_ptr<DiagnosticSink> diagnostics);
    void report(OperationKind operation, ApproximationKind approximation,
                std::string reason) const;

    std::shared_ptr<DomainBackend> backend_;
    std::shared_ptr<DiagnosticSink> diagnostics_;
};

std::shared_ptr<Manager>
makeOctagonManager(const OctagonOptions& options = {});

class AbstractState
{
public:
    AbstractState(const AbstractState& rhs);
    AbstractState(AbstractState&& rhs) noexcept;
    AbstractState& operator=(const AbstractState& rhs);
    AbstractState& operator=(AbstractState&& rhs) noexcept;
    ~AbstractState();

    const Environment& environment() const { return environment_; }
    const std::shared_ptr<const Manager>& manager() const { return manager_; }

    void assign(Variable target, const LinearExpression& expression);
    void assign(Variable target, const TreeExpression& expression);
    void assume(const LinearConstraint& constraint);
    void assume(const TreeConstraint& constraint);
    void forget(Variable variable);

    AbstractState joined(const AbstractState& other) const;
    AbstractState met(const AbstractState& other) const;
    AbstractState widened(const AbstractState& next,
                          const WideningPolicy& policy = {}) const;
    AbstractState narrowed(const AbstractState& next) const;
    AbstractState projectLowerBounds() const;
    AbstractState changedEnvironment(const Environment& environment,
                                     bool projectNewVariables = false) const;

    void joinWith(const AbstractState& other);
    void meetWith(const AbstractState& other);
    void widenWith(const AbstractState& next,
                   const WideningPolicy& policy = {});
    void narrowWith(const AbstractState& next);
    void changeEnvironment(const Environment& environment,
                           bool projectNewVariables = false);

    bool isBottom() const;
    bool isTop() const;
    CheckResult leq(const AbstractState& other) const;
    CheckResult equals(const AbstractState& other) const;
    CheckResult entails(const LinearConstraint& constraint) const;

    Interval bound(Variable variable) const;
    Box toBox() const;
    ConstraintSet toConstraints() const;
    std::string toString() const;

private:
    friend class Manager;
    AbstractState(std::shared_ptr<const Manager> manager,
                  Environment environment,
                  std::unique_ptr<BackendState> state);
    const DomainBackend& backend() const;
    DomainBackend& backend();
    void requireCompatible(const AbstractState& other) const;

    std::shared_ptr<const Manager> manager_;
    Environment environment_;
    std::unique_ptr<BackendState> state_;
};

} // namespace relational

#endif // RELATIONAL_DOMAIN_H
