# Oh 2012 packed sparse numerical kernel

Experimental opt-in API in `AE/Core/PackedSparseAnalysis.h`, with a test/measurement executable `PackedSparseAnalysisTest`. The production `ae` executable does not select this engine yet. The existing Dense/Semi-Sparse implementations and baselines are untouched.

Reference: Oh et al., *Design and Implementation of Sparse Global Analyses for C-like Languages*, PLDI 2012, sections 2 and 4--6, https://kihongheo.kaist.ac.kr/publications/pldi12.pdf .

## Contract

- Mathematical integers, explicit scalar and singleton memory-cell variables. No LLVM wrap, alias discovery, call adapter, exceptions, or widening. Exceeding `maximumRounds` throws, never returns a purported invariant.
- Fixed possibly overlapping packs; missing variables become singleton packs. Candidates exceeding the default cap of 10 split into sorted consecutive chunks. This deterministic splitting policy is ours. Candidate extraction from C syntax is still pending; LLVM basic blocks do not reproduce C lexical blocks.
- A pack has its own SVF native `OctagonDomain`, with ComponentDense/COW/incremental closure. Physical components inside this domain are independent of the static logical packs.
- Assignment/havoc updates all packs containing the target. Assume updates all packs containing an operand; variables outside each target pack are imported as independent interval temporaries and projected away after transfer. Assignment imports the same way. Each imported interval is the intersection of bounds from all its owner packs. All writes use a pre-state snapshot.
- Pack states form an unreduced product. There is no automatic cross-pack relational reduction. Bottom in one pack does not automatically overwrite every other pack. Such extra reductions would require additional dependencies. Entry initializes every pack to top, all other slots start bottom. The transfer is strict on its own pack; unreachable components remain bottom. Queries distinguish observed bottom from Safe.
- Approximate Def is also in Use, preserving the old pack through potential/spurious writes and weak updates. External operand owners also occur in Use. Query is an identity-definition/use of all its operand packs. The runtime checks actual reads against this contract.
- Graph generation uses backwards reaching definitions, treating every approximate Def as a kill, as permitted in section 5. This is a correctness-first implementation, not the paper's SSA/BDD optimization. No numeric fixpoint is required to construct the graph.
- Dense retains all node/pack slots; Sparse retains only definitions. Both solve the same packed equations with ordered sweeps and exact convergence tests. No widening is used, so iterations may differ without changing the final least solution.
- Validation independently expands omitted identity equations over the original CFG, checks every original packed equation and compares all Dense/Sparse pack states. It does not trust compiled dependency routes. Mutation tests bypass an assume edge and must be rejected. This checks instances, not a universal soundness proof of the numerical library/frontend.

## Precision example

With packs `{x,y}` and `{y,o}`, `y=x+1; assume(x<=5)` refines `y<=6` in the first pack. The second pack can still contain `y<=11`. `o=y` inside the second pack therefore need not prove `o<=6`. An assignment `z=y` into a singleton `{z}` imports the intersection of both y bounds and can prove `z<=6`. The tests retain both outcomes. Removing that difference requires an explicit reduction design.

## Reproduce

Configure the ordinary SVF CMake build with LLVM, Z3, GMP and MPFR, then:

```sh
cmake --build build-packed --target PackedSparseAnalysisTest -j 4
ctest --test-dir build-packed -R PackedSparseAnalysis --output-on-failure
python3 tests/ae-relational/run_packed_sparse_benchmark.py \
  build-packed/bin/PackedSparseAnalysisTest /absolute/path/to/new-results
```

The runner requires a new output directory, preserves raw attempts, validates a deliberately invalid dependency graph, and performs one warmup plus three measurements for 8/32/64 packs of two variables. Dense and Sparse use identical graph/query identities and packing. It records setup, dependency construction, solve, verification and total times, and peak RSS before/after verification. Microsecond/millisecond local measurements diagnose the mechanism; they do not establish real-program throughput or a scale-up frontier. Existing v1/v2 pilot records remain separate from later measurements.

## Production gates still open

Syntax-driven candidate extraction, ICFG/call and memory-effect adaptation, machine arithmetic, convergence policy, fixed-query real-program comparison, and a non-relational baseline all require additional implementation and validation. Do not present these tests as benchmark-bc measurements or as a fix for the existing production Semi-Sparse engine.
