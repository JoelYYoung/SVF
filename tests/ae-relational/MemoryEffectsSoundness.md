# Integer-derived pointers and external memory copies

The shared AE transfer used to give `inttoptr` an empty address set. This
silently discarded stores and lost alternatives in mixed pointers. The new
transfer returns address Top, or exact null for a proven zero. Pointer/integer
provenance is not recovered, so even a known roundtrip may lose precision.
The change is local to AE; Andersen's independent points-to result is unchanged.
D3 must reject or conservatively invalidate a runtime target outside its
static version model; it must not publish accepted results after such rejection.

`PointerRoundTripStoreWitness.ll` is a defined pointer-sized roundtrip that writes
7 over 1. Its first concrete assertion is false. `MixedPointerRoundTripWitness.ll`
is an independently reviewed variant where one branch writes q and leaves o=1.
Both previously produced false Safe across Box/whole/packed Dense/Semi/Oh.
The frozen supervisor R05 input is retained separately as
`UnknownTargetStoreWitness.ll`; it does not itself establish a valid concrete
dereference on every target platform.

The old memcpy model treated byte indices as SVF field indices and could miss
a scalar destination entirely. Exact memcpy/memmove and their recognized LLVM
forms now copy a complete same-type scalar object when size, base and strong
update eligibility are established. `ExternalCopyWitness.ll` requires May for
the stale value and Safe for the copied value. All other layouts conservatively
havoc every represented destination-allocation field; an unknown destination
havocs modeled memory. This deliberately loses partial-byte/cross-field facts.
Unknown lengths cannot justify skipping writes. A proven zero length is a no-op
only for recognized byte-copy APIs. MEMCPY-tagged functions such as strncpy and
memccpy also use the conservative fallback because padding/stopping and argument
conventions differ.

`MemoryEffectsSoundnessTest` tests six inputs in seven Box/whole/packed
Dense/Semi/Oh configurations with explicit expected outcomes, query identities
and original-equation Post. Partial, zero, optional-length and ambiguous-target
copies are covered, as are strncpy and memccpy annotation boundaries.

These are shared frontend/model repairs, not Sparse improvements. Passing Post
does not prove a shared transfer's concrete soundness. Unmodeled external writes
to globals, general byte-memory semantics, and all possible alias/effect cases
remain outside this targeted regression evidence.
