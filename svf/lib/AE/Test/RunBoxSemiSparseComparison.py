#!/usr/bin/env python3

"""Compare legacy IntervalState and Box-backed Semi-Sparse AE end to end."""

import argparse
import csv
import os
import pathlib
import platform
import re
import signal
import statistics
import subprocess
import tempfile
import time


MODES = ("legacy", "box")
FIELDS = (
    "input",
    "mode",
    "repetition",
    "status",
    "seconds",
    "peak_rss_bytes",
    "analyzed_nodes",
    "return_code",
    "diagnostic",
)


def parse_input(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("input must be LABEL=PATH")
    label, path = value.split("=", 1)
    if not label or not pathlib.Path(path).is_file():
        raise argparse.ArgumentTypeError(f"invalid benchmark input: {value}")
    return label, path


def peak_rss(text):
    if platform.system() == "Darwin":
        match = re.search(
            r"^\s*(\d+)\s+maximum resident set size$", text, re.MULTILINE
        )
        return int(match.group(1)) if match else None
    match = re.search(
        r"^\s*Maximum resident set size \(kbytes\):\s*(\d+)$",
        text,
        re.MULTILINE,
    )
    return int(match.group(1)) * 1024 if match else None


def command(options, mode, input_path):
    if mode == "legacy":
        runner = options.legacy_runner
        extapi = options.legacy_extapi
        mode_options = (
            "-ae-dense-octagon=false",
            "-ae-sparsity=semi-sparse",
            "-ae-sparse-legacy-interval=true",
        )
    else:
        runner = options.box_runner
        extapi = options.box_extapi
        mode_options = ("-ae-sparsity=semi-sparse",)
    return [runner, *mode_options, "-stat=false", f"-extapi={extapi}", input_path]


def run_once(options, mode, input_path):
    time_flag = "-l" if platform.system() == "Darwin" else "-v"
    with tempfile.NamedTemporaryFile() as time_output:
        measured = [
            "/usr/bin/time",
            time_flag,
            "-o",
            time_output.name,
            *command(options, mode, input_path),
        ]
        start = time.perf_counter()
        process = subprocess.Popen(
            measured,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
        try:
            output, _ = process.communicate(timeout=options.timeout)
            status = "pass" if process.returncode == 0 else "fail"
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate()
            status = "timeout"
        seconds = time.perf_counter() - start
        time_output.seek(0)
        rss = peak_rss(time_output.read().decode(errors="replace"))

    match = re.search(r"AE_GENERIC_OBSERVATION analyzed_nodes=(\d+)", output)
    nodes = int(match.group(1)) if match else ""
    lines = output.rstrip().splitlines()
    diagnostic = lines[-1] if lines else ""
    return {
        "status": status,
        "seconds": f"{seconds:.6f}",
        "peak_rss_bytes": rss if rss is not None else "",
        "analyzed_nodes": nodes,
        "return_code": process.returncode,
        "diagnostic": diagnostic,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--legacy-runner", required=True)
    parser.add_argument("--box-runner", required=True)
    parser.add_argument("--legacy-extapi", required=True)
    parser.add_argument("--box-extapi", required=True)
    parser.add_argument("--input", action="append", type=parse_input,
                        required=True, metavar="LABEL=PATH")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--require-node-match", action="store_true")
    parser.add_argument("--require-all-pass", action="store_true")
    parser.add_argument("--output", required=True)
    options = parser.parse_args()
    if options.repetitions <= 0 or options.timeout <= 0:
        parser.error("repetitions and timeout must be positive")

    rows = []
    for input_index, (label, input_path) in enumerate(options.input):
        observations = {mode: [] for mode in MODES}
        for repetition in range(1, options.repetitions + 1):
            order = MODES if (input_index + repetition) % 2 else MODES[::-1]
            for mode in order:
                observation = run_once(options, mode, input_path)
                observation.update(
                    input=label, mode=mode, repetition=repetition
                )
                observations[mode].append(observation)
                rows.append(observation)
        legacy_nodes = {row["analyzed_nodes"] for row in observations["legacy"]}
        box_nodes = {row["analyzed_nodes"] for row in observations["box"]}
        if options.require_all_pass:
            failures = [
                f"{row['mode']}#{row['repetition']}={row['status']}"
                for mode in MODES
                for row in observations[mode]
                if row["status"] != "pass"
            ]
            if failures:
                raise RuntimeError(f"{label}: " + ", ".join(failures))
        if options.require_node_match and (
            "" in legacy_nodes
            or "" in box_nodes
            or len(legacy_nodes) != 1
            or legacy_nodes != box_nodes
        ):
            raise RuntimeError(
                f"{label}: analyzed-node mismatch "
                f"legacy={sorted(legacy_nodes)} box={sorted(box_nodes)}"
            )
        summaries = []
        for mode in MODES:
            passed = [row for row in observations[mode]
                      if row["status"] == "pass"]
            times = [float(row["seconds"]) for row in passed]
            rss_values = [int(row["peak_rss_bytes"]) for row in passed
                          if row["peak_rss_bytes"] != ""]
            summaries.append(
                f"{mode}:time={statistics.median(times):.3f}s,"
                f"rss={statistics.median(rss_values) / (1024 * 1024):.1f}MiB"
                if times and rss_values else f"{mode}:no completed samples"
            )
        print(f"{label:16s} {' '.join(summaries)}", flush=True)

    output_path = pathlib.Path(options.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
