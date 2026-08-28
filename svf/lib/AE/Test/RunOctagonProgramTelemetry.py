#!/usr/bin/env python3

"""Collect production Octagon operation telemetry from LLVM modules."""

import argparse
import csv
import os
import pathlib
import re
import subprocess
import tempfile
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
    parser.add_argument("--sparsity", action="append",
                        choices=("dense", "semi-sparse", "sparse"))
    parser.add_argument("--limit", type=int, default=64)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--operations-output", required=True)
    parser.add_argument("--runs-output", required=True)
    options = parser.parse_args()
    sparsities = options.sparsity or ["dense", "semi-sparse", "sparse"]

    operation_rows = []
    run_rows = []
    with tempfile.TemporaryDirectory(prefix="octagon-telemetry-") as temporary:
        for label, path in options.input:
            for sparsity in sparsities:
                telemetry_path = pathlib.Path(temporary) / f"{label}-{sparsity}.csv"
                environment = os.environ.copy()
                environment["SVF_OCTAGON_TELEMETRY"] = str(telemetry_path)
                command = [
                    options.runner,
                    f"-ae-sparsity={sparsity}",
                    "-ae-dense-octagon=true",
                    f"-ae-dense-octagon-max-dimensions={options.limit}",
                    "-widen-delay=1",
                    "-stat=false",
                    f"-extapi={options.extapi}",
                    str(path),
                ]
                start = time.perf_counter()
                try:
                    result = subprocess.run(
                        command, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True,
                        timeout=options.timeout, check=False, env=environment,
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
                dimensions = int(match.group(2)) if match else ""
                selected = int(match.group(4)) if match else ""
                rows = []
                if telemetry_path.is_file():
                    with telemetry_path.open(newline="") as telemetry_file:
                        rows = list(csv.DictReader(telemetry_file))
                for row in rows:
                    operation_rows.append({
                        "input": label,
                        "path": str(path),
                        "bytes": path.stat().st_size,
                        "sparsity": sparsity,
                        "selected_dimensions": dimensions,
                        **row,
                    })
                run_rows.append({
                    "input": label,
                    "path": str(path),
                    "bytes": path.stat().st_size,
                    "sparsity": sparsity,
                    "limit": options.limit,
                    "dimensions": dimensions,
                    "selected": selected,
                    "status": status,
                    "seconds": f"{elapsed:.6f}",
                    "telemetry_rows": len(rows),
                    "operation_calls": sum(int(row["count"]) for row in rows),
                })
                print(f"{label:20s} {sparsity:11s} dimensions={dimensions!s:>4s} "
                      f"selected={selected!s:>1s} status={status:7s} "
                      f"calls={run_rows[-1]['operation_calls']}")

    if not operation_rows:
        raise RuntimeError("no Octagon operation telemetry was produced")
    operations_path = pathlib.Path(options.operations_output)
    operations_path.parent.mkdir(parents=True, exist_ok=True)
    with operations_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file,
                                fieldnames=operation_rows[0].keys())
        writer.writeheader()
        writer.writerows(operation_rows)
    runs_path = pathlib.Path(options.runs_output)
    runs_path.parent.mkdir(parents=True, exist_ok=True)
    with runs_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=run_rows[0].keys())
        writer.writeheader()
        writer.writerows(run_rows)


if __name__ == "__main__":
    main()
