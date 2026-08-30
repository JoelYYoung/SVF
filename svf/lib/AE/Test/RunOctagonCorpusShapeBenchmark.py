#!/usr/bin/env python3

"""Replay real-program vocabulary shapes through Octagon storage candidates.

The input CSV supplies each program's maximum Environment and maximum naive
relation-pack size.  The benchmark constructs a conservative uniform-block
proxy with those two values.  It compares physical carriers under identical
Octagon closure semantics; it is not an end-to-end program trace replay.
"""

import argparse
import csv
import pathlib
import subprocess
import time


SCHEMES = ("dense-half", "sparse-finite", "component-dense")
WORKLOADS = {
    "lookup": (200000, 5),
    "copy-update": (20, 5),
    "close": (1, 3),
}
BENCHMARK_FIELDS = (
    "scheme", "workload", "topology", "variables", "component_size",
    "operations", "repetitions", "median_ns_per_op", "p95_ns_per_op",
    "finite_slots", "allocated_bound_slots", "component_count", "checksum",
    "peak_rss_bytes",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", required=True)
    parser.add_argument("--vocabulary-csv", type=pathlib.Path, required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    options = parser.parse_args()
    if options.timeout <= 0:
        parser.error("timeout must be positive")

    with options.vocabulary_csv.open(newline="") as input_file:
        shapes = list(csv.DictReader(input_file))

    rows = []
    for shape_index, shape in enumerate(shapes):
        variables = int(shape["env_max"])
        component_size = max(1, int(shape["naive_pack_max"]))
        for workload_index, (workload, workload_config) in enumerate(
                WORKLOADS.items()):
            operations, repetitions = workload_config
            offset = (shape_index + workload_index) % len(SCHEMES)
            schemes = SCHEMES[offset:] + SCHEMES[:offset]
            for scheme in schemes:
                command = [
                    options.benchmark,
                    "--scheme", scheme,
                    "--workload", workload,
                    "--topology", "block",
                    "--variables", str(variables),
                    "--component-size", str(component_size),
                    "--operations", str(operations),
                    "--repetitions", str(repetitions),
                    "--seed", "20260830",
                ]
                start = time.perf_counter()
                try:
                    result = subprocess.run(
                        command, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True,
                        timeout=options.timeout, check=False,
                    )
                    elapsed = time.perf_counter() - start
                    status = "pass" if result.returncode == 0 else "fail"
                    output = result.stdout.strip()
                    return_code = result.returncode
                except subprocess.TimeoutExpired as error:
                    elapsed = time.perf_counter() - start
                    status = "timeout"
                    output = error.stdout or ""
                    if isinstance(output, bytes):
                        output = output.decode(errors="replace")
                    output = output.strip()
                    return_code = ""

                row = {
                    "input": shape["input"],
                    "source_environment": variables,
                    "source_max_pack": component_size,
                    "status": status,
                    "wall_seconds": f"{elapsed:.6f}",
                    "return_code": return_code,
                    "diagnostic": "",
                }
                if status == "pass":
                    values = next(csv.reader([output.splitlines()[-1]]))
                    if len(values) != len(BENCHMARK_FIELDS):
                        raise RuntimeError(
                            f"unexpected benchmark output for {shape['input']}: "
                            f"{output}"
                        )
                    row.update(dict(zip(BENCHMARK_FIELDS, values)))
                else:
                    row.update({field: "" for field in BENCHMARK_FIELDS})
                    row["scheme"] = scheme
                    row["workload"] = workload
                    row["topology"] = "block"
                    row["variables"] = variables
                    row["component_size"] = component_size
                    row["operations"] = operations
                    row["repetitions"] = repetitions
                    row["diagnostic"] = output.splitlines()[-1] if output else ""
                rows.append(row)
                print(
                    f"{shape['input']:16s} {workload:11s} {scheme:15s} "
                    f"status={status:7s} wall={elapsed:.3f}s",
                    flush=True,
                )

    fieldnames = [
        "input", "source_environment", "source_max_pack", "status",
        "wall_seconds", "return_code", *BENCHMARK_FIELDS, "diagnostic",
    ]
    options.output.parent.mkdir(parents=True, exist_ok=True)
    with options.output.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
