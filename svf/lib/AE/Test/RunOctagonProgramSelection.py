#!/usr/bin/env python3

"""Measure whether real LLVM modules can select the production Octagon.

The integration runner builds SVFIR and the call graph, reports the largest
production Environment, and exits before abstract execution.  This separates
domain-selection eligibility from end-to-end analyzer performance.
"""

import argparse
import csv
import os
import pathlib
import re
import subprocess
import time


SELECTION = re.compile(
    r"OCTAGON_SELECTION sparsity=(\S+) dimensions=(\d+) "
    r"limit=(\d+) selected=(\d+)"
)


def parse_input(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("input must be LABEL=PATH")
    label, raw_path = value.split("=", 1)
    path = pathlib.Path(raw_path)
    if not label or not path.is_file():
        raise argparse.ArgumentTypeError(f"invalid benchmark input: {value}")
    return label, path.resolve()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--extapi", required=True)
    parser.add_argument("--input", action="append", type=parse_input,
                        required=True, metavar="LABEL=PATH")
    parser.add_argument("--sparsity", default="dense",
                        choices=("dense", "semi-sparse", "sparse"))
    parser.add_argument("--limit", type=int, default=64)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.limit < 0 or options.timeout <= 0:
        parser.error("limit must be non-negative and timeout positive")

    rows = []
    for label, path in options.input:
        environment = os.environ.copy()
        environment["SVF_OCTAGON_TELEMETRY"] = "stderr"
        environment["SVF_OCTAGON_SELECTION_ONLY"] = "1"
        command = [
            options.runner,
            f"-ae-sparsity={options.sparsity}",
            "-ae-dense-octagon=true",
            f"-ae-dense-octagon-max-dimensions={options.limit}",
            "-stat=false",
            f"-extapi={options.extapi}",
            str(path),
        ]
        start = time.perf_counter()
        try:
            result = subprocess.run(
                command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True, timeout=options.timeout, check=False,
                env=environment,
            )
            elapsed = time.perf_counter() - start
            status = "pass" if result.returncode == 0 else "fail"
            output = result.stdout
        except subprocess.TimeoutExpired as error:
            elapsed = time.perf_counter() - start
            status = "timeout"
            output = error.stdout or ""
            if isinstance(output, bytes):
                output = output.decode(errors="replace")

        match = SELECTION.search(output)
        if status == "pass" and not match:
            raise RuntimeError(f"{label}: selection observation missing\n{output}")
        dimensions = int(match.group(2)) if match else ""
        selected = int(match.group(4)) if match else ""
        rows.append({
            "input": label,
            "path": str(path),
            "bytes": path.stat().st_size,
            "sparsity": options.sparsity,
            "limit": options.limit,
            "dimensions": dimensions,
            "selected": selected,
            "status": status,
            "seconds": f"{elapsed:.6f}",
        })
        print(f"{label:28s} dimensions={dimensions!s:>6s} "
              f"selected={selected!s:>1s} status={status}")

    output_path = pathlib.Path(options.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
