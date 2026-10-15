//===- NumericalOperationTrace.h -- Opt-in numerical workload trace -*- C++
//-*-===//

#ifndef SVF_AE_NUMERICAL_OPERATION_TRACE_H
#define SVF_AE_NUMERICAL_OPERATION_TRACE_H

#include "AE/Core/NumericalDomain.h"

#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace SVF::AbstractDomain
{

/// Versioned, append-only trace sink for real numerical-domain workloads.
/// The trace is deliberately opt-in: constructing no writer leaves the normal
/// production domain path untouched. Each operation records independent
/// before/after mathematical snapshots, so later replay does not have to infer
/// state ownership from aggregate telemetry.
class NumericalOperationTraceWriter final
{
public:
    explicit NumericalOperationTraceWriter(const std::string& path);
    ~NumericalOperationTraceWriter();

    /// Temporarily suppress rows while preserving the tracing decorator type.
    /// This is used by semantic validation replays, whose operations are not
    /// part of the measured production workload.
    void suspend();
    void resume();

    NumericalOperationTraceWriter(const NumericalOperationTraceWriter&) =
        delete;
    NumericalOperationTraceWriter& operator=(
        const NumericalOperationTraceWriter&) = delete;

private:
    friend class TracingNumericalDomain;

    void recordCreate(std::uint64_t state, const NumericalDomain& value);
    void recordClone(std::uint64_t source, std::uint64_t destination,
                     const NumericalDomain& value);
    void recordDestroy(std::uint64_t state);
    void recordOperation(const char* operation, std::uint64_t state,
                         std::uint64_t rhs, const std::string& payload,
                         const NumericalDomain* before,
                         const NumericalDomain* rhsValue,
                         const NumericalDomain* after,
                         const std::string& result,
                         const OperationMetadata* metadata);
    std::uint64_t allocateState();
    void writeRow(const char* event, std::uint64_t state, std::uint64_t rhs,
                  const std::string& operation, const std::string& payload,
                  const std::string& before, const std::string& rhsSnapshot,
                  const std::string& after, const std::string& result,
                  const OperationMetadata* metadata);

    std::ofstream output_;
    std::mutex mutex_;
    std::uint64_t nextSequence_ = 1;
    std::uint64_t nextState_ = 1;
    unsigned suspensionDepth_ = 0;
};

/// Wrap a domain with complete interpreter-facing operation tracing. The
/// returned wrapper owns `domain` and shares `writer` across all clones.
std::unique_ptr<NumericalDomain> traceNumericalDomain(
    std::unique_ptr<NumericalDomain> domain,
    std::shared_ptr<NumericalOperationTraceWriter> writer);

} // namespace SVF::AbstractDomain

#endif // SVF_AE_NUMERICAL_OPERATION_TRACE_H
