#!/usr/bin/env python3

"""Compare legacy IntervalState and native Box AE storage end to end.

The runner must be svf-ae-relational-integration-test. Inputs are supplied by
the caller so the same harness works for checked-in fixtures and external real
program bitcode. Each invocation is a fresh process, preventing singleton or
allocator state from leaking between modes.
"""

import argparse
import csv
import pathlib
import re
import statistics
import subprocess
import time


MODES = {
    "dense-legacy": ["-ae-sparsity=dense", "-ae-dense-legacy-interval=true"],
    "dense-native": ["-ae-sparsity=dense", "-ae-dense-legacy-interval=false"],
    "semi-legacy": [
        "-ae-sparsity=semi-sparse",
        "-ae-sparse-legacy-interval=true",
    ],
    "semi-native": [
        "-ae-sparsity=semi-sparse",
        "-ae-sparse-legacy-interval=false",
    ],
    "full-legacy": ["-ae-sparsity=sparse", "-ae-sparse-legacy-interval=true"],
    "full-native": ["-ae-sparsity=sparse", "-ae-sparse-legacy-interval=false"],
}


def parse_input(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("input must be LABEL=PATH")
    label, path = value.split("=", 1)
    if not label or not pathlib.Path(path).is_file():
        raise argparse.ArgumentTypeError(f"invalid benchmark input: {value}")
    return label, path


def run_once(options, mode, input_path):
    command = [
        options.runner,
        "-ae-dense-octagon=false",
        *MODES[mode],
        "-stat=false",
        f"-extapi={options.extapi}",
        input_path,
    ]
    start = time.perf_counter()
    try:
        result = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=options.timeout,
            check=False,
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

    match = re.search(r"AE_GENERIC_OBSERVATION analyzed_nodes=(\d+)", output)
    nodes = int(match.group(1)) if match else ""
    return status, elapsed, nodes, output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--extapi", required=True)
    parser.add_argument("--input", action="append", type=parse_input,
                        required=True, metavar="LABEL=PATH")
    parser.add_argument("--mode", action="append", choices=MODES,
                        help="default: all six modes")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.repetitions <= 0 or options.timeout <= 0:
        parser.error("repetitions and timeout must be positive")

    modes = options.mode or list(MODES)
    rows = []
    for label, input_path in options.input:
        for mode in modes:
            samples = []
            statuses = []
            node_counts = []
            for repetition in range(1, options.repetitions + 1):
                status, elapsed, nodes, output = run_once(
                    options, mode, input_path
                )
                samples.append(elapsed)
                statuses.append(status)
                node_counts.append(nodes)
                rows.append(
                    {
                        "input": label,
                        "mode": mode,
                        "repetition": repetition,
                        "status": status,
                        "seconds": f"{elapsed:.6f}",
                        "analyzed_nodes": nodes,
                    }
                )
                if status == "fail":
                    raise RuntimeError(
                        f"{label}/{mode} failed:\n{output.rstrip()}"
                    )
            completed = [
                sample
                for sample, status in zip(samples, statuses)
                if status == "pass"
            ]
            summary = (
                f"median={statistics.median(completed):.3f}s"
                if completed else f">={options.timeout:.1f}s"
            )
            print(
                f"{label:20s} {mode:12s} {summary} "
                f"status={','.join(statuses)} nodes={node_counts[0]}"
            )

    output_path = pathlib.Path(options.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
