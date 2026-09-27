# Shared-callee caller-frame preservation (R14)

Status: candidate; independent review and production acceptance are pending.
Parent is 0b742b2d. This changes the common Semi/D3 restore hook, used both
by solving and Dense Post replay. A passing replay is not an independent
proof of the new return semantics.

The first full gate run failed on CallerFrameCase2 in both parent and
candidate. Under handle-recur=top Dense/Oh report false Safe with passing Post;
Semi reports Safe with failing Post; the old D3 reports May with failing Post.
The concrete companion returns 99, so g==1 is false. skipRecursionWithTop
scans only the direct function's basic blocks, and additionally excludes
constant/pointer RHS stores. This is a separate common recursion-summary
defect; its repair is not part of this candidate. Keep the expected May and
the failing test. Do not claim this candidate has passed its acceptance gates.

## Sufficient safety argument

Consider a call in function F and the set of scalar SSA names available there
and defined in F. Assume the modeled execution is sequential, uses ordinary
matched call/return, and cannot execute F again before this call returns.
Each selected name denotes the same value at call and return. For a concrete
return state, the projection of its scalar valuation therefore belongs to the
call-site projection. It also belongs to the sound callee-exit abstraction.
Intersecting these two sets contains every return state. The argument uses
only immutable caller scalars; memory is taken exclusively from the exit.

A context-insensitive exit joins all represented call contexts. It contains
the returns of this particular call as a subset. Intersecting that union with
the call-site scalar projection does not remove an actual return of this call.
Other contexts can make the result less precise. They cannot make a fact true
in this call unless it already holds in every retained represented state.
Names unavailable at the call or belonging to another function are excluded
from the new preservation rule and retain the old forget/assign behavior.
The argument assumes each transfer/join respects missing-coordinate-as-Top
and the existing scalar-to-activation correspondence. It is not a proof of
the full interpreter, its entry coverage, or the memory effect model.

## Executable admission check and fallback

The check traverses the entire transitive callee closure from this call.
Reaching F rejects preservation. Resolved indirect calls are traversed only
when all points-to objects are resolved functions; empty, black-hole, and
other targets reject it. External functions reject it, with one explicit
test-harness exception: `nondet_i32` is a fresh-input primitive that never
calls back. This is a harness semantic contract, not an inference from the
symbol name for arbitrary user code. No libc whitelist is installed.
In particular printf, qsort, unknown callbacks, and unresolved indirect calls
keep the old path. The result is cached per static return site after call-graph
construction. `SVF_AE_TRACE_CALLER_FRAME=1` records the decision.

For admitted caller scalars, restore missing product facets and meet unary
bounds without forgetting their callee-exit relations. Meet the existing
call-site scalar-only projection. Never meet caller memory or content-version
coordinates. The callee can write globals and reachable objects; R14n tests
that an old caller-to-memory relation is not resurrected after such a write.
Recursion through F retains the old path for every recursion policy. The Top
recursion model and external callback coverage remain separate limitations.

libproxy's print_proxies calls printf. Without a reviewed external no-callback
contract this patch intentionally does not certify its two remaining D3 Post
failures. No failure is hidden by dropping a check or relabeling it May.

## Gates

- R14, R14n with same caller; different callers sharing a callee with different
  content; a transitive call back to F; resolved and unknown indirect calls;
  an external callback candidate. Keep the negative queries May.
- Record admission decisions and fixed query IDs, not just alarm totals.
- Run E/R, witnesses and libproxy; record conservative refusal and Post Fail.
- Independent Supervisor D3 integration, including their dominator-availability
  revision, remains separate from this common-layer tree.
