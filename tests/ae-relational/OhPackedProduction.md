# Conservative Oh-style production prototype

Select `-ae-domain=octagon -ae-backend=native
-ae-relational-policy=syntax-pack -ae-sparsity=oh-packed` and provide
`-ae-post-check=... -ae-query-input-id=...`. Other domains/backends and the
numerical trace decorator are rejected. This is a separate execution class
from both existing SemiSparse and the standalone mathematical-integer kernel.

## Contract

- Fixed syntax packs are abstract locations. A local single-predecessor
  binary/copy/compare statement defines every pack containing its result.
- Joins, conditional incoming edges, calls/returns, load/store, phi, entry,
  and unclassified effects define all packs. Residual single-predecessor
  cycles get a conservative all-pack barrier.
- Static backward routes bypass identity locations and stop at a pack
  definition. Every pack is currently a Use. There is no claimed Use pruning.
- Persist only declared pack definitions; the Box fallback, initialization,
  addresses and lifetime flow densely. Reads reconstruct from the routes;
  ordinary transfers still materialize the full packed state. The original
  WTO/call/widening driver is retained.
- Before eliding a pack, check equality with its routed source. An omitted
  write or global Bottom change fails closed. Original Dense-equation Post
  checking is mandatory; this per-transfer check alone does not establish
  consistency after later definition updates.
- A mutable state borrow invalidates reconstructed caches. Old read objects
  remain alive within a transfer; callers must own a clone across node/merge
  boundaries, as the shared driver does. Caches are cleared at boundaries.
- `AE_OH_STORAGE pack_slots` counts persistent definitions ONLY. Complete
  temporary states, read caches, O(nodes*packs) route metadata and the Dense
  Post replay also consume memory. Peak read-cache slots are reported; use
  end-to-end peak RSS for total memory. No acceleration claim is made.

## Regressions

`ctest --test-dir BUILD -R 'Packed|OhPacked' --output-on-failure` runs numerical
tests and the production regression script. The latter compares Loop, Call
and SharedCallee ledgers with same-pack Dense, checks original Post equations,
exercises cache invalidation after a routed definition changes, and deliberately
drops a definition to require rejection. Raw files use a new temporary directory.

`SVF_AE_OH_DROP_DEFINITION=1` and `SVF_AE_OH_CACHE_CHECK=1` are test-only hooks.
The mutation must never be used as an accepted analysis configuration.

The external Supervisor E1--E13 suite has 36 fixed queries. E14 reuses E04;
E15 uses E04 with pack cap=2 (expected Safe-to-May packing loss in both Dense
and this prototype). These and the six real programs require separate raw
records. Tests do not constitute a general frontend soundness proof.

## Remaining work

Nontrivial pack Use pruning / demand-local transient transfer, more compact
dependency metadata, independent review, six-program measurement and final
report. This conservative prototype is not a completed optimized Oh solver.
