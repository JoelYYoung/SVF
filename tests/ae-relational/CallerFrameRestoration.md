# Shared-callee caller-frame preservation (R14)

Status: candidate; independent review and production acceptance are pending.
Rebased parent is 62b89b7a; original candidate 65b046aa is retained. This changes the common Semi/D3 restore hook, used both
by solving and Dense Post replay. A passing replay is not an independent
proof of the new return semantics.

The original full gate run failed on CallerFrameCase2 in both old parent and
candidate. Under handle-recur=top Dense/Oh report false Safe with passing Post;
Semi reports Safe with failing Post; the old D3 reports May with failing Post.
The concrete companion returns 99, so g==1 is false. skipRecursionWithTop
scans only the direct function's basic blocks, and additionally excludes
constant/pointer RHS stores. This is a separate common recursion-summary
defect, subsequently repaired in bd0b7dba/62b89b7a. Keep the original failed
artifacts and the expected May. Re-run the gate on this repaired parent.
The common CTest now selects the five non-D3 configurations explicitly;
the old D3 code in this branch lacks the Supervisor's version-routing repairs
and is not a valid integrated D3 candidate. Use --configs including d3,pack-d3
after integration; D3 and libproxy acceptance remain separate open gates.

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
test-harness exception enabled only by `-ae-harness-nondet-no-callback=true`:
`nondet_i32` is a fresh-input primitive that never calls back. The option is
false by default. It is an explicit harness premise, not an inferred property
of an arbitrary external symbol. No libc whitelist is installed.
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

`preservesCallerFrame(returnSite) const` is the protected shared admission
predicate for derived executors. For a shared callee it delegates to exactly
the closure check used by production restoration; non-shared sites retain
their existing rule. It does not authorize preservation of foreign function
coordinates or unavailable names. A derived D3 conservative fallback must
weaken the production state and subsequent materialization/query semantics,
and reconstruct that same state for Post. Weakening only the checker's final
state while queries keep stronger facts is not acceptable. Version summaries
must not silently re-import relations intentionally discarded at the boundary.
Such a D3 fallback is a separate change with precision-loss accounting.

## Gates

- R14, R14n with same caller; different callers sharing a callee with different
  content; a transitive call back to F; resolved and unknown indirect calls;
  an external callback candidate. Keep the negative queries May.
- Record admission decisions and fixed query IDs, not just alarm totals.
- Run E/R, witnesses and libproxy; record conservative refusal and Post Fail.
- Independent Supervisor D3 integration, including their dominator-availability
  revision, remains separate from this common-layer tree.

## Repaired-parent evidence

The old candidate and its failed recursion gate remain at 65b046aa. This tree
reuses its frozen R12/R14 inputs byte-for-byte, including their documented
extra final blank line; no input identity is silently rewritten for whitespace.
The only cherry-pick conflict was the CMake test list; both suites are kept.

`caller-frame-repaired-before-v1`: 35 parent runs. The initial candidate v1
also passed 35 runs, but its implicit name-based harness premise was then
replaced by the default-off option; v1 is preserved as a development attempt.
Final `caller-frame-repaired-gates-v2` (explicit premise) and
`caller-frame-repaired-default-v2` (no premise) each passed 35/35, each with
Post 1130 Pass / 170 Unreachable. With the premise, the two R14 queries in
whole-semi and pack-semi improve from May to Safe. Without it they stay May.
Negative overwrite, distinct-caller and recursion cases keep their expected
outcomes. Unknown external and indirect cases reject frame preservation.

Final E/R 138 runs and witness 108 runs have unchanged query identities and
outcomes against 62b89b7a. libproxy Box/packed-Dense/Semi/Oh all complete, each
61 queries and Post 158 Pass / 1 Infeasible / 1 Unreachable. These are
non-D3 regressions; they do not certify D3's two outstanding return edges.
CTest-v2: 15 Pass / 1 Skip, Release, WARN_AS_ERROR=OFF, ELINA disabled.
No sanitizer rerun for this R14 change is claimed. Original equations share
the new hook, so independent review and explicit counterexamples remain
necessary even when Post passes.
