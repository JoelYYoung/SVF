#!/usr/bin/env python3

"""Measure relational vocabulary pressure on real LLVM modules.

The integration runner builds SVFIR and Andersen, reports carrier vocabulary,
operation-support, alias, call-interface, and naive pack-connectivity metrics,
then exits before abstract execution.  The CSV is intended to distinguish
semantic vocabulary pressure from the physical storage density of a domain.
"""

import argparse
import csv
import os
import pathlib
import subprocess
import time


PREFIX = "RELATIONAL_VOCAB_AUDIT "


def parse_input(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("input must be LABEL=PATH")
    label, raw_path = value.split("=", 1)
    path = pathlib.Path(raw_path)
    if not label or not path.is_file():
        raise argparse.ArgumentTypeError(f"invalid benchmark input: {value}")
    return label, path.resolve()


def parse_observation(output):
    for line in output.splitlines():
        if not line.startswith(PREFIX):
            continue
        fields = {}
        for item in line[len(PREFIX):].split():
            key, value = item.split("=", 1)
            fields[key] = int(value)
        return fields
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--extapi", required=True)
    parser.add_argument("--input", action="append", type=parse_input,
                        required=True, metavar="LABEL=PATH")
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.timeout <= 0:
        parser.error("timeout must be positive")

    rows = []
    metric_names = None
    for label, path in options.input:
        environment = os.environ.copy()
        environment["SVF_OCTAGON_TELEMETRY"] = "stderr"
        environment["SVF_OCTAGON_SELECTION_ONLY"] = "1"
        environment["SVF_RELATIONAL_VOCABULARY_AUDIT"] = "1"
        command = [
            options.runner,
            "-ae-sparsity=dense",
            "-ae-dense-octagon=true",
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

        metrics = parse_observation(output)
        if status == "pass" and metrics is None:
            raise RuntimeError(f"{label}: vocabulary observation missing\n{output}")
        if metrics is not None:
            if metric_names is None:
                metric_names = list(metrics)
            elif list(metrics) != metric_names:
                raise RuntimeError(f"{label}: metric schema changed")
        row = {
            "input": label,
            "path": str(path),
            "bytes": path.stat().st_size,
            "status": status,
            "seconds": f"{elapsed:.6f}",
        }
        if metric_names is not None:
            for name in metric_names:
                row[name] = metrics[name] if metrics is not None else ""
        rows.append(row)
        print(
            f"{label:36s} status={status:7s} "
            f"env_max={row.get('env_max', '')!s:>6s} "
            f"pack_max={row.get('naive_pack_max', '')!s:>6s} "
            f"call_objects={row.get('call_direct_objects_max', '')!s:>6s}"
        )

    if metric_names is None:
        metric_names = []
    output_path = pathlib.Path(options.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = ["input", "path", "bytes", "status", "seconds"] + metric_names
    with output_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
