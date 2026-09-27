# Entry-reachable scalar availability (R08)

Parent: `12a57147` (R10 repair). R08 has `main -> f` and an uncalled `g -> f`.
With main-entry analysis the old scalar must-availability intersection included
g's empty state. f then lost the global address `@o`: Semi produced one Post
failure, and D3 rejected a store with unknown target.

The new prepass starts at the global node plus exactly `collectProgEntryFuns()`.
It takes a static fixed point over Intra/Call/Ret edges, requiring a return edge's
call site to be reachable. It makes no assumption about branch feasibility.
Only reachable predecessors participate in the existing must-availability
intersection. Selected root entries also receive the implicit global-state edge
used by execution, even when no explicit ICFG edge exists. Variables are still
computed by the existing maximal fixed-point set intersection; this change is
independent of the D3 dominator optimization.

Reachability is defined by the current frontend and entry/environment model.
It cannot establish coverage of missing callbacks or unknown external entries.
The no-main policy selects entry SCCs, not every exported function. It is not
silently replaced by main when building this graph.

## Regression boundaries

- R08 main entry: eight configurations complete; Post 29 Pass / 4 Unreachable
  each. Semi's failures and D3's store rejection disappear. Both query outcomes
  remain May, including the ideal-Safe roundtrip. There is no claimed precision
  improvement on that query.
- R08 no-main: main and g are both roots. All six test configurations reach the
  two queries. The shared Post checker explicitly rejects multiple entries with
  one Unsupported record. The test verifies this refusal; no-main semantics are
  **not** certified. The original expectation that this checker could certify
  multiple entries failed and is preserved in ctest-v3.log.
- Fixed E/R (23 x 8): 182 completed, two expected D3 R07 refusals, Post 5268 Pass.
  18 witnesses x 8: 144 completed, Post 2976/74/246. Query IDs and outcomes are
  unchanged against the parent, covering existing loops and call/return cases.
- libproxy: Box/Dense/Semi/Oh complete; D3 still times out at the 60-second
  diagnostic bound. This tree retains the old D3 implementation; it does not
  include Supervisor's new deterministic version-availability optimization.

The six-program final common-binary matrix and independent review remain open.
The historical R10 erratum stays in force pending re-evaluation.
