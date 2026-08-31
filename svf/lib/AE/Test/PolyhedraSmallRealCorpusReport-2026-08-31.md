# Production Polyhedra small-real-program feasibility benchmark

Date: 2026-08-31

## Question and validity boundary

This experiment asks whether native `ConvexPolyhedraState` can complete the
actual SVF abstract-execution pipeline on non-synthetic programs, and how its
end-to-end resource use changes with environment size.  It is not a claim that
Box and Polyhedra provide equal precision.  Box is a deployment baseline used
to distinguish frontend/pointer-analysis cost from relational-domain cost.

The experiment became meaningful only after commit `acc2e04c` connected
Polyhedra to the production Dense, Semi-Sparse, and Full-Sparse abstract
interpreters.  Earlier Polyhedra/APRON measurements exercised public domain
operations but did not execute program transfer and fixpoint computation.

## Inputs

The bitcode is the reproducible `llvm14-small-real` lane at benchmark-bc commit
`6eba5d4`.  It contains upstream curl and zlib examples and the cJSON test
driver linked with the real cJSON implementation.  Source commits, build flags,
SHA-256 values, and the rebuild script are recorded in that repository's
README.

| Input | LLVM instruction lines | Analyzed nodes | Selected dimensions |
| --- | ---: | ---: | ---: |
| curl `simple.c` | 48 | 57 | 51 |
| zlib `zpipe.c` | 324 | 356 | 156 |
| cJSON `test.c + cJSON.c` | 6,965 | 2,635 for completed Box run | 1,019 |

## Protocol

- Machine: Apple M5 MacBook Pro, 10 cores, 32 GiB RAM, macOS 26.6.2.
- Runner: `RunRelationalCarrierCorpusBenchmark.py`.
- Each measurement uses a fresh analyzer process.
- Candidate order rotates across repetitions.
- Polyhedra selection is probed separately and is not included in timed runs.
- Dimension admission limit: 100,000, so all observed inputs select Polyhedra.
- Successful curl and zlib cases use three repetitions.
- cJSON uses one 300-second / 24-GiB feasibility run.  Its timeout remains a
  right-censored observation; it is not converted into a fabricated runtime.
- Peak RSS is `/usr/bin/time` maximum RSS for completed runs and sampled
  process-group RSS for the killed timeout.

## Results

Values below are medians of three successful runs unless marked otherwise.

| Input | Candidate | Status | Time (s) | Peak RSS (MiB) |
| --- | --- | --- | ---: | ---: |
| curl-simple | Box | pass | 0.024128 | 10.92 |
| curl-simple | Polyhedra | pass | 0.184162 | 28.78 |
| zlib-zpipe | Box | pass | 0.033768 | 14.70 |
| zlib-zpipe | Polyhedra | pass | 114.401966 | 866.94 |
| cjson-test | Box, one run | pass | 12.945074 | 93.97 |
| cjson-test | Polyhedra, one run | timeout | greater than 300 | 174.56 at termination |

Derived comparisons:

| Input | Polyhedra / Box time | Polyhedra / Box peak RSS |
| --- | ---: | ---: |
| curl-simple | 7.63x | 2.64x |
| zlib-zpipe | 3,387.88x | 58.96x |
| cjson-test | greater than 23.18x | not comparable after timeout |

The raw rows, including order, repetition, RSS source, selected dimensions,
return code, and right-censored status, are in
`PolyhedraSmallRealCorpusResults-2026-08-31.csv`.

## Interpretation

1. Production Polyhedra abstract interpretation now exists and completes on
   real programs; the previous absence of a production interpreter was the
   reason a Polyhedra whole-program benchmark could not honestly be reported.
2. Environment size alone is insufficient as a cost model.  Moving from 51 to
   156 dimensions changes Polyhedra time by roughly three orders of magnitude,
   while Box remains below 0.04 seconds.
3. A sparse coefficient row carrier can reduce unused coefficient storage, but
   cannot by itself eliminate exact Polyhedra's facet/generator growth, H/V
   conversion, redundancy checks, or GMP rational cost.
4. Future carrier telemetry must therefore record both physical storage events
   and semantic representation pressure: H rows, V generators, nonzero
   coefficients, conversion calls, redundancy-elimination calls, and maximum
   intermediate counts.
5. Pack/environment partitioning and physical sparse storage are complementary.
   The observed 1,019-dimension cJSON environment makes an unrestricted global
   exact Polyhedra state an unsuitable default even before the 24-hour large
   corpus results are available.

## Large-program extension

The original seven-program corpus is running from clean commit `acc2e04c` on
O3 and O4.  Each case has a 24-hour timeout and a 256-GiB process-group memory
limit.  Two workers run per host, so the configured worst-case concurrent
memory is at most 512 GiB per 1-TiB host.  Results are written under:

```text
/mnt/scratch/PAG/yjw/projects/polyhedra-corpus-2026-08-31/
```

The final report must preserve timeout and memory-limit outcomes and must not
replace them with the previous Octagon shape proxy.
