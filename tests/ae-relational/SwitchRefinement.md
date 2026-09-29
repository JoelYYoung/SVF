# R23 switch refinement contract

The LLVM builder preserves each target's complete case set, condition width,
and whether the target also includes default. The legacy successor integer is
retained for compatibility but is not an AE switch equality constraint.

For 32--64-bit conditions, explicit-only targets admit the signed case hull.
Default targets (including explicit/default shared targets), missing case sets,
and widths outside 32--64 receive **no switch refinement**. Narrow values may
have different signed/zero-extended representations; skipping refinement is the
conservative fallback, not a claim that every narrow transfer is correct.
Memory refinement additionally retains the existing fresh-load, single-target,
strong-store requirements. Constraints are met, not assigned. Scalar switch
conditions do not require a defining PAG edge (external returns can lack one).

Persistent gates, all with Post and final-ledger recheck:

| Input | Expected assertions / structural gate |
|---|---|
| R23 | lines 9/10/11 Safe, line 12 May; packed D3 retains four May |
| Scalar (Supervisor S6) | lines 8/9/10 Safe, line 11 May |
| S1 | four targets: {-1}, {1,2}, {3}, default; width 32 |
| Narrow | trunc i32 200 to i8 and its load: both reachable false assertions May; no Infeasible Post edge |
| Wide | i128 case/default assertions May |
| DefaultShared | default + case 1 target assertion May |

Eight configurations: Box, whole Dense/Semi/D3, packed Dense/Semi/D3 and Oh.
`SVF_AE_TRACE_SWITCH_CASES` emits read-only IR case metadata for S1; it is not
a new Post identity schema. Test output retains all commands, ledgers, Post,
stdout/stderr, binary hashes and failures. `--expect-old` only changes R23
expectations; the full suite is not expected to pass on the old binary.

Known limits: the 7aa528db-based packed D3 result remains less precise than
whole D3 for R23. This change does not import d3-next. CDGBuilder still consumes
legacy switch labels and is **not repaired here**. Stable Post equation keys,
ASan, wide-machine-integer semantics and a general soundness proof remain
outside this local regression gate. Old experiment matrices are not rewritten.
