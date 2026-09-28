# External memory-query roles

The MEMCPY annotation covers regular copy functions and the legacy iconv model.
Argument count does not identify a calling convention. Regular copies inspect
pointer arguments 0 and 1; iconv retains roles 1, 2, 3 and 4. Model-role/type
mismatches fail closed. The iconv test checks the existing query schema, not
the completeness of iconv reset/nullable-buffer semantics.

`check_memcpy_queries.py` checks Box Dense, whole Dense/Semi, packed Dense/Semi
and Oh. Safe copies must have two Safe pointer queries per call. Null-destination
copies must have destination May and source Safe. Neither scalar length nor
scalar flags are null-dereference queries. The malformed ABI must exit 2.

`--reference-before` captures the old defect intentionally; running without
that switch against the old binary is the red test. All ledgers and commands
are kept in the output directory. New destination query IDs are added; the old
integer-argument IDs are removed. This is a query-schema correction, not a
precision gain on a fixed query set. Global-node detection is a separate change.
