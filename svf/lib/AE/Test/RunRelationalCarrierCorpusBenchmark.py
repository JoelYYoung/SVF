#!/usr/bin/env python3

"""Screen Box, Octagon, and Polyhedra on real LLVM modules.

This runner is deliberately an end-to-end feasibility screen, not a claim
that domains have equal precision.  Each candidate runs in a fresh process.
Candidate order rotates within each repetition, and timeout/OOM outcomes
remain in the output as right-censored observations.
"""

import argparse
import csv
import os
import pathlib
import platform
import re
import signal
import subprocess
import tempfile
import threading
import time


FIELDNAMES = [
    "input", "path", "candidate", "repetition", "status", "seconds",
    "peak_rss_bytes", "rss_source", "sampled_peak_rss_bytes", "rss_samples",
    "selected_dimensions", "dimension_limit", "octagon_selected",
    "polyhedra_selected",
    "analyzed_nodes", "return_code", "telemetry_path", "diagnostic",
]


def parse_input(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("input must be LABEL=PATH")
    label, raw_path = value.split("=", 1)
    path = pathlib.Path(raw_path)
    if not label or not path.is_file():
        raise argparse.ArgumentTypeError(f"invalid benchmark input: {value}")
    return label, path.resolve()


def parse_peak_rss(text):
    if platform.system() == "Darwin":
        match = re.search(r"^\s*(\d+)\s+maximum resident set size$", text,
                          re.MULTILINE)
        return int(match.group(1)) if match else None
    match = re.search(
        r"^\s*Maximum resident set size \(kbytes\):\s*(\d+)$", text,
        re.MULTILINE,
    )
    return int(match.group(1)) * 1024 if match else None


def sample_process_group_rss(process_group, stop_event, result,
                             memory_limit_bytes):
    while not stop_event.is_set():
        sample = subprocess.run(
            ["ps", "-axo", "pgid=,rss="], stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, check=False,
        )
        rss_kib = 0
        for line in sample.stdout.splitlines():
            columns = line.split()
            if (len(columns) == 2 and columns[0].isdigit() and
                    int(columns[0]) == process_group):
                rss_kib += int(columns[1])
        result[0] = max(result[0], rss_kib * 1024)
        result[1] += 1
        if (memory_limit_bytes and result[0] > memory_limit_bytes and
                not result[2]):
            result[2] = True
            try:
                os.killpg(process_group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        stop_event.wait(0.1)


def parse_selection(output, domain):
    prefix = "OCTAGON" if domain == "dense-octagon" else "POLYHEDRA"
    match = re.search(
        rf"{prefix}_SELECTION\s+.*dimensions=(\d+)\s+limit=(\d+)\s+"
        rf"selected=(\d+)", output,
    )
    if not match:
        return None, None, None
    return int(match.group(1)), int(match.group(2)), int(match.group(3))


def probe_dimensions(options, input_path, domain):
    environment = os.environ.copy()
    if domain == "dense-octagon":
        environment["SVF_OCTAGON_TELEMETRY"] = "stderr"
    else:
        environment["SVF_POLYHEDRA_TELEMETRY"] = "stderr"
    environment["SVF_RELATIONAL_SELECTION_ONLY"] = "1"
    command = [
        options.runner, "-ae-sparsity=dense",
        "-ae-dense-octagon=" +
        ("true" if domain == "dense-octagon" else "false"),
        f"-ae-dense-octagon-max-dimensions={options.dimension_limit}",
        "-ae-dense-polyhedra=" +
        ("true" if domain == "dense-polyhedra" else "false"),
        f"-ae-dense-polyhedra-max-dimensions={options.dimension_limit}",
        f"-ae-fun-entry={options.fun_entry}", "-stat=false",
        f"-extapi={options.extapi}", str(input_path),
    ]
    result = subprocess.run(
        command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        timeout=min(options.timeout, 30.0), check=False, env=environment,
    )
    if result.returncode != 0:
        raise RuntimeError(f"dimension probe failed for {input_path}\n{result.stdout}")
    dimensions, limit, selected = parse_selection(result.stdout, domain)
    if dimensions is None:
        raise RuntimeError(f"dimension observation missing for {input_path}")
    return dimensions, limit, selected


def run_once(options, label, input_path, candidate, repetition,
             expected_selection):
    telemetry_path = ""
    environment = os.environ.copy()
    octagon = candidate == "dense-octagon"
    polyhedra = candidate == "dense-polyhedra"
    if octagon:
        telemetry_path = str(
            options.telemetry_directory /
            f"{label}-{candidate}-r{repetition}.csv"
        )
        environment["SVF_OCTAGON_TELEMETRY"] = telemetry_path
    else:
        environment.pop("SVF_OCTAGON_TELEMETRY", None)
    if polyhedra:
        environment["SVF_POLYHEDRA_TELEMETRY"] = "stderr"
    else:
        environment.pop("SVF_POLYHEDRA_TELEMETRY", None)

    time_flag = "-l" if platform.system() == "Darwin" else "-v"
    with tempfile.NamedTemporaryFile() as time_output:
        command = [
            "/usr/bin/time", time_flag, "-o", time_output.name,
            options.runner,
            "-ae-sparsity=dense",
            "-ae-dense-legacy-interval=false",
            f"-ae-dense-octagon={'true' if octagon else 'false'}",
            f"-ae-dense-octagon-max-dimensions={options.dimension_limit}",
            f"-ae-dense-polyhedra={'true' if polyhedra else 'false'}",
            f"-ae-dense-polyhedra-max-dimensions={options.dimension_limit}",
            "-ae-box-directory-cow=false",
            "-ae-box-hash-directory-cow=false",
            f"-ae-fun-entry={options.fun_entry}",
            "-stat=false",
            f"-extapi={options.extapi}",
            str(input_path),
        ]
        start = time.perf_counter()
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, start_new_session=True, env=environment,
        )
        stop_event = threading.Event()
        sampled_peak = [0, 0, False]
        sampler = threading.Thread(
            target=sample_process_group_rss,
            args=(process.pid, stop_event, sampled_peak,
                  options.memory_limit_bytes),
        )
        sampler.start()
        try:
            output, _ = process.communicate(timeout=options.timeout)
            if sampled_peak[2]:
                status = "memory-limit"
            else:
                status = "pass" if process.returncode == 0 else "fail"
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate()
            status = "timeout"
        stop_event.set()
        sampler.join()
        elapsed = time.perf_counter() - start
        time_output.seek(0)
        measured_peak = parse_peak_rss(
            time_output.read().decode(errors="replace")
        )

    peak_rss = measured_peak if measured_peak is not None else sampled_peak[0]
    rss_source = "time" if measured_peak is not None else (
        "process-group-sample" if sampled_peak[0] else ""
    )
    dimensions, dimension_limit, selected = (None, None, None)
    if octagon or polyhedra:
        dimensions, dimension_limit, selected = parse_selection(
            output, candidate
        )
    if (octagon or polyhedra) and dimensions is None:
        dimensions, dimension_limit, selected = expected_selection
    nodes_match = re.search(r"AE_GENERIC_OBSERVATION analyzed_nodes=(\d+)",
                            output)
    diagnostic_lines = output.rstrip().splitlines()
    return {
        "input": label,
        "path": str(input_path),
        "candidate": candidate,
        "repetition": repetition,
        "status": status,
        "seconds": f"{elapsed:.6f}",
        "peak_rss_bytes": peak_rss or "",
        "rss_source": rss_source,
        "sampled_peak_rss_bytes": sampled_peak[0],
        "rss_samples": sampled_peak[1],
        "selected_dimensions": dimensions if dimensions is not None else "",
        "dimension_limit": dimension_limit if dimension_limit is not None else "",
        "octagon_selected": (
            selected if octagon and selected is not None else ""
        ),
        "polyhedra_selected": (
            selected if polyhedra and selected is not None else ""
        ),
        "analyzed_nodes": int(nodes_match.group(1)) if nodes_match else "",
        "return_code": process.returncode,
        "telemetry_path": telemetry_path,
        "diagnostic": diagnostic_lines[-1] if diagnostic_lines else "",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--extapi", required=True)
    parser.add_argument("--input", action="append", type=parse_input,
                        required=True, metavar="LABEL=PATH")
    parser.add_argument("--candidate", action="append",
                        choices=("box", "dense-octagon", "dense-polyhedra"))
    parser.add_argument("--fun-entry", choices=("main", "no-main"),
                        default="main")
    parser.add_argument("--dimension-limit", type=int, default=4096)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--memory-limit-gib", type=float, default=24.0)
    parser.add_argument("--telemetry-directory", type=pathlib.Path,
                        required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    options = parser.parse_args()
    if (options.dimension_limit <= 0 or options.repetitions <= 0 or
            options.timeout <= 0 or options.memory_limit_gib < 0):
        parser.error("limits and repetitions must be positive")
    options.memory_limit_bytes = int(
        options.memory_limit_gib * 1024 * 1024 * 1024
    )
    options.telemetry_directory.mkdir(parents=True, exist_ok=True)
    candidates = options.candidate or [
        "box", "dense-octagon", "dense-polyhedra"
    ]

    rows = []
    relational_candidates = [
        candidate for candidate in candidates if candidate != "box"
    ]
    selections = {
        (label, candidate): probe_dimensions(options, path, candidate)
        for label, path in options.input
        for candidate in relational_candidates
    }
    for input_index, (label, path) in enumerate(options.input):
        for repetition in range(1, options.repetitions + 1):
            offset = (input_index + repetition - 1) % len(candidates)
            order = candidates[offset:] + candidates[:offset]
            for candidate in order:
                expected = selections.get((label, candidate), (None, None, None))
                row = run_once(options, label, path, candidate, repetition,
                               expected)
                rows.append(row)
                rss_mib = (float(row["peak_rss_bytes"]) / (1024 * 1024)
                           if row["peak_rss_bytes"] else 0.0)
                print(
                    f"{label:16s} {candidate:14s} r={repetition} "
                    f"status={row['status']:12s} time={row['seconds']}s "
                    f"rss={rss_mib:.1f}MiB dims={row['selected_dimensions']}",
                    flush=True,
                )

    options.output.parent.mkdir(parents=True, exist_ok=True)
    with options.output.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=FIELDNAMES)
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
