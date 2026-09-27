# Shared weak-store regression

`MultiTargetStoreWitness.ll` is the frozen supervisor R03 input (SHA-256
`41aa9d32dd58d1a60317f4e1a165f76218d416a6a34e9d6f4973f04a109e66ea`).
It contains three assertions: `y >= 100`, `y >= 5`, `y <= 200`.
After `o=5; p = choice ? &o : &q; *p=v`, with `v` in `[100,200]`,
the branch `p=&q` retains `o=5`. Therefore the first assertion must be May.

At 7156c568 every Octagon configuration, including packed Dense and Oh,
incorrectly reported all three Safe. All Post equations passed because the
checker replayed the same erroneous shared store transfer. Box returned May
for all three in this input. Passing Post cannot validate the concrete meaning
of a common transfer function.

The repair retains the unchanged state on each possible target using
`join(S, assign(S, cell, value))`. This preserves initialization and address
alternatives along with numerical values. Sequential weak updates conservatively
approximate the target choice; they do not retain all cross-target correlations.
Strong updates require a singleton, live, non-heap/non-array/non-blackhole,
field-sensitive cell outside recursive stack frames. The later affine store
refinement uses the same eligibility guard. An unregistered pointer uses the
same weak/strong rules instead of bypassing them.

`ctest --test-dir BUILD -R WeakStoreSoundnessTest --output-on-failure` checks
seven Box/whole/packed Dense/Semi/Oh configurations with fixed query identities,
explicit semantic outcomes and Post. R03 must be May; Octagon must retain the
two valid bounds. Existing fixtures test definite stores separately.

Scope: this repairs the demonstrated multi-target overwrite error. Heap/array,
recursive frames, unknown/external effects and escaped addresses still require
broader concrete-semantics validation. This test is not a universal soundness
proof. It does not close the E12a relational weak-update precision gap.
