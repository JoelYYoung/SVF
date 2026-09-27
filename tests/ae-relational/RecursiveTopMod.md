# Recursive TOP effect contract (R15)

Candidate based on 0b742b2d, independent of the unaccepted R14 return patch.
The old summary missed constant and pointer-valued writes, transitive callees,
and targets expressed through callee-local pointers. It also skipped all
effects when the return node had multiple outgoing edges. Old Dense replay
used the same wrong transfer, so its successful Post was not independent
evidence for those effects. Frozen R15a-g come from Supervisor fixtures 451955e.

## Static contract

`recursiveTopMod(call)` is a deterministic, abstract-state-independent query
after preAnalysis has constructed Andersen and the call graph. It walks every
resolved callee and the whole transitive call closure, including all members
of recursive SCCs. It examines every StoreStmt regardless of RHS kind.
Each Andersen target contributes its complete base-object region. Empty,
non-object or black-hole targets, unresolved indirect calls and unmodeled
external functions produce allObjects. The default external rule includes
global writes, callbacks and lifetime effects; absence of explicit pointer
arguments does not establish purity. No libc or nondet exception is installed.

`recursiveTopSummary(call)` identifies modeled TOP entry calls from outside
a recursive SCC (any resolved recursive target suffices). It does not assert
actual runtime execution or complete resolution of external callbacks. MOD
covers the whole callee even if an acyclic prefix precedes the recursive WTO
cycle and has already executed. Duplicate conservative havoc is allowed.

## Runtime contract

Havoc all currently represented fields of the selected regions in both
numerical and address facets. New schema coordinates have implicit Top until
assigned. Join the pre-havoc state to retain any uninitialized alternative:
MOD denotes may-write, not definitely initialized. Under allObjects also
forget lifetime information. Return values include pointer and numeric forms;
return-node out-degree never bypasses effects. SSA values unrelated to the
return and memory retain their pre-call meaning under the existing call model.

The soundness argument is conditional on the existing frontend and Andersen
target overapproximation: actual writes lie within MOD, other scalar values
are unchanged, and affected values can be arbitrary. Whole-region Top covers
each actual field update and all skipped control paths. Unknown effects use
allObjects rather than an empty effect. This does not prove completeness of
frontend effects such as unsupported IR, asynchronous signals, or callbacks.

Versioned executors must use the same MOD during version construction, remove
invalid reaching versions at the matching return, and validate the runtime
`prepareRecursiveHavoc(call,bases,all)` hook. The common tree leaves the old
D3 fail-closed checks in place; Supervisor owns the version-graph integration.
A D3 rejection is not a May conclusion or a completed analysis.

`SVF_AE_TRACE_RECURSIVE_TOP=1` emits each actual summary invocation. Post replay
can invoke it too; run without Post to measure solver-only trigger counts.
Historical Post "recursive body ... top" markers are static classifications,
not invocation counts. Six-program trigger/affected-query audits remain open.

## Post summary edge

For a stored direct TOP call and its return, Post availability flows directly
from the call to the return and adds the actual return definition. No skipped
callee exit contributes to this summary path. An explicit `recursive-summary`
obligation checks havoc(call-state) against the reconstructed return-state.
The ordinary next-node equation then observes the caller's available addresses.
Semi's static scalar availability uses the same direct summary predecessor
and does not import the skipped callee's formal-return ghost. Static reachability
also includes this synthetic edge, even if the original exit has no path.
This repairs the missing synthetic edge, not the semantics of a shared callee.
Indirect mixed-target summary edges remain outside this new Post rule; D3
rejects recursive indirect calls under its own contract.

## Tests and retained failures

- R15a-g negative queries must be May in completed configurations.
- RecursiveModCase1 is a no-write-to-g positive; case2 covers pointer return,
  case3 unknown external global writes, case4 unknown integer-derived target.
- RecursiveModCase5 preserves a caller SSA load across a recursive write to
  its former memory source; its query must remain Safe. Case5 was added with
  the explicit scalar-availability summary edge after the 66-run v2 gate.
- Independent D3 version integration, multi-outgoing-return runtime coverage,
  and full fixed/real-program regression are separate gates, not presumed.
- First build failed because PointerAnalysis::inSameCallGraphSCC is not const;
  the query now uses the existing mutable analysis pointer without altering
  the points-to graph. build-v1.log is retained.
- First 66-run gate (`recursive-mod-gates-v1`) had 20 Semi Post failures.
  R15d trace showed that memory survived at the return, but its global address
  was omitted by Post availability. After the explicit summary edge, v2 passes
  all 66 runs (11 inputs, six non-D3 configurations). Old D3 is intentionally
  excluded from this gate until its version graph consumes the static API.

After the Semi availability change, `recursive-mod-gates-v3` passed 72/72
(12 inputs, same six configurations). `recursive-mod-er-v2` completed 138/138
with 3978 Pass; `recursive-mod-witness-v2` completed 108/108 with Post
2245 Pass / 53 Infeasible / 180 Unreachable. Query IDs/outcomes were unchanged
against the corresponding v1 batches and their pre-repair common-input rows.
CTest (`ctest-v2.log`) was 14 Pass / 1 Skip. ELINA was disabled, so adapter
stub successes do not constitute ELINA numerical coverage. These parallel
local runs are semantic regressions, not isolated performance measurements.
No sanitizer or final six-program matrix is claimed here.
