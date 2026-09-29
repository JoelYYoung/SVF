# Global detector query schema v2 (candidate)

This change derives from cbe2057e. Frozen historical ledgers are not rewritten.

| Kind | Property | One-past | Unknown allocation extent |
| --- | --- | --- | --- |
| gep-allocation-range | Formed byte offset belongs to [0, allocation size] | Safe | Unsupported |
| load-bounds / store-bounds | [offset, offset + LLVM store-size) belongs to allocation | May for positive width | Unsupported |
| load-address / store-address / external argument | Nullness/lifetime check on an actual access | Not a bounds certificate | Conservative semantics |

GEP does not access memory. The old gep-address null-dereference query is removed.
The range property is diagnostic, not a complete LLVM inbounds/poison or C
subobject/provenance certificate. An out-of-range non-inbounds GEP need not be
undefined. A zero or absent extent is Unsupported, including unknown externs.
Allocation-range Safe does not certify an ensuing dereference.

LLVM reference: https://llvm.org/docs/GetElementPtr.html#rules

Global initializers execute their transfers followed by detectors, before entry
functions. The global final remains subject to the existing Post equation.
New statement queries bind input_id + v2 + detector + semantic_site + kind.
LLVM instruction sites use function/basic-block operand/instruction ordinal;
global constant expressions use their typed LLVM operand text and synthetic GEP
field. LLVM-uniqued identical constant expressions represent one property, not
one query per textual use. Different statement pointers with the same semantic
key fail closed, rather than silently dropping a query. PAG allocation IDs are
diagnostic only. This metadata currently requires the LLVM frontend; serialized
PAG inputs without it are rejected for these queries.

Acceptance remains OPEN: build, R18 four distinct sites, positive/negative access
tests, repeated cross-configuration IDs, global execution outcomes and Post,
old/new ledgers, and independent Supervisor review are required. Full memory
safety (alignment, subobjects, poison, external models, all instruction kinds)
is outside this local repair; no universal soundness claim is made.

`GlobalQueryNegative.ll` is the unchanged R21 IR from independent supervisor
fixtures b085ce4. Its guarded negative index must yield May for both the formed
allocation range and actual load; the null-field load remains May after removal
of gep-address. Four synthetic initializer field sites must remain distinct.
The old numerical address-carrier clamp is not a detector bound. The candidate
uses unclamped byte offsets and joins its offset side table conservatively.

Store-address is new coverage, not a replacement count for removed gep-address.
New load/store bounds and store-address verdicts are in the query ledger; they
do not currently add entries to the legacy bug-reporter output. That output is
not used as the complete query set. Atomic and other unenumerated memory
instructions remain outside this local query-coverage contract.
