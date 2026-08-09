//===- AbstractState.h -- Common abstract-state lattice API -----*- C++ -*-===//

#ifndef SVF_AE_ABSTRACT_STATE_H
#define SVF_AE_ABSTRACT_STATE_H

#include <memory>
#include <string>

namespace SVF
{

enum class CheckResult
{
    False,
    True,
    Unknown
};

const char* toString(CheckResult result);

/// Common value-level interface implemented by every complete abstract state.
///
/// It deliberately contains only lattice operations.  Transfer languages are
/// domain-specific: IntervalState is keyed by SVF variable/object ids, whereas
/// OctagonState consumes Environment/LinearExpression/LinearConstraint.
class AbstractState
{
public:
    virtual ~AbstractState();

    virtual std::unique_ptr<AbstractState> clone() const = 0;
    virtual const char* name() const = 0;

    std::unique_ptr<AbstractState> joined(const AbstractState& other) const;
    std::unique_ptr<AbstractState> met(const AbstractState& other) const;
    std::unique_ptr<AbstractState> widened(const AbstractState& next) const;
    std::unique_ptr<AbstractState> narrowed(const AbstractState& next) const;

    void joinWith(const AbstractState& other);
    void meetWith(const AbstractState& other);
    void widenWith(const AbstractState& next);
    void narrowWith(const AbstractState& next);

    bool isBottom() const;
    bool isTop() const;
    CheckResult leq(const AbstractState& other) const;
    CheckResult equals(const AbstractState& other) const;
    std::string toString() const;

protected:
    AbstractState() = default;
    AbstractState(const AbstractState&) = default;
    AbstractState(AbstractState&&) noexcept = default;
    AbstractState& operator=(const AbstractState&) = default;
    AbstractState& operator=(AbstractState&&) noexcept = default;

    void requireCompatible(const AbstractState& other) const;

private:
    virtual bool hasCompatibleDomain(const AbstractState& other) const = 0;
    virtual void joinState(const AbstractState& other) = 0;
    virtual void meetState(const AbstractState& other) = 0;
    virtual void widenState(const AbstractState& next) = 0;
    virtual void narrowState(const AbstractState& next) = 0;
    virtual bool isBottomState() const = 0;
    virtual bool isTopState() const = 0;
    virtual bool leqState(const AbstractState& other) const = 0;
    virtual std::string stateToString() const = 0;
};

} // namespace SVF

#endif // SVF_AE_ABSTRACT_STATE_H
