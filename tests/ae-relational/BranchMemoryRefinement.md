# Branch-to-memory refinement contract (R09/R10)

Reference `20d73d8c` could use the value of an old load to narrow overwritten
memory. R10 has the concrete counterexample x=0, v=1000 and must report May.
The same incorrect transfer passed its own Dense Post replay. This fix does
not use Post success as an independent proof of the memory semantics.

Refinement is admitted only for a singleton, non-summary target satisfying the
existing strong-store predicate, on a single unconditional intra-block chain
from the load through the branch, with no store/call. Reaching the comparison
alone is insufficient. Direct loads and same-type identity copies are supported;
other casts, unsigned and floating comparisons conservatively skip this memory
refinement. Unknown aliases, heap/array/field-insensitive/recursive-stack objects
and uncertain lifetime also skip it. Scalar guard handling is otherwise unchanged.

An admitted bound is intersected using numerical assume, preserving relations
and initialization. It does not assign the content coordinate. This is a local
sufficient freshness proof, not MemorySSA or a complete alias-sensitive proof.
Switch load freshness follows the same rule; general switch-edge encoding is
unchanged and is not certified by these tests.

`BranchMemoryRefinementTest` retains 35 runs over Box, whole Dense/Semi and packed
Dense/Oh. R09 is Safe/May after repair; R10's false-Safe query must be May. Its
upper-bound positive is an ideal Safe, but may remain May (Box ignores the scalar
input guards). The fresh relation positive requires Safe in whole Dense/Semi;
Box and limited packs keep unresolved positives explicitly. Extra cases cover
write after compare, intervening call, non-singleton load and switch case.

Rebuild generated LLVM fixtures with LLVM 21:

```
clang -O0 -g -DCASE=1 -S -emit-llvm BranchMemoryRefinement.c -o case.raw.ll
opt -S -passes=mem2reg case.raw.ll -o BranchMemoryCase1.ll
```

Repeat CASE 1..5. R09/R10 LLVM fixtures are frozen copies of Supervisor probes.
Their input hashes and full query identities are retained in run artifacts.
The first candidate runner incorrectly demanded ideal Safe positives for Box
and small packs; its failed expectations and outputs are preserved, not counted
as analyzer soundness failures.

R12 from Supervisor fixtures d22f6b8 is now frozen here: cases 1,2,3,7 are
independent old-false-Safe/new-May red tests for call, non-singleton load,
stale switch, and unsigned-as-signed respectively. The earlier
BranchMemoryCase1/2/4/5 were already May on 20d73d8c; they provide coverage
but are not red tests. Cases 4/9 (cast), 5 (fresh unsigned), and 8 (cross-BB,
Box/packed) explicitly record real Safe-to-May precision costs; case 6 is a
fresh switch positive. The runner now records 80 runs and separates these
roles. Case 4's source comment says overwrite w, but the actual store is to o;
the statements are retained without repair. Imported C/LLVM copies add one
final blank line; their recorded hashes identify these exact local inputs.

R08 (unreachable caller and scalar availability) is a separate reviewed change.
No six-program rerun or new scale/precision certification is implied here.
