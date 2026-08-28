#!/usr/bin/env python3

"""Measure end-to-end AE time and peak RSS for legacy/native carriers."""

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
import threading
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

FIELDNAMES = [
    "input",
    "mode",
    "repetition",
    "status",
    "seconds",
    "peak_rss_bytes",
    "rss_source",
    "sampled_peak_rss_bytes",
    "rss_samples",
    "analyzed_nodes",
    "return_code",
]


def parse_input(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("input must be LABEL=PATH")
    label, path = value.split("=", 1)
    if not label or not pathlib.Path(path).is_file():
        raise argparse.ArgumentTypeError(f"invalid benchmark input: {value}")
    return label, path


def parse_peak_rss(text):
    if platform.system() == "Darwin":
        match = re.search(r"^\s*(\d+)\s+maximum resident set size$", text,
                          re.MULTILINE)
        return int(match.group(1)) if match else None
    match = re.search(
        r"^\s*Maximum resident set size \(kbytes\):\s*(\d+)$",
        text,
        re.MULTILINE,
    )
    return int(match.group(1)) * 1024 if match else None


def sample_process_group_rss(process_group, stop_event, result):
    while not stop_event.is_set():
        sample = subprocess.run(
            ["ps", "-axo", "pgid=,rss="],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            check=False,
        )
        rss_kib = 0
        for line in sample.stdout.splitlines():
            columns = line.split()
            if len(columns) == 2 and columns[0].isdigit() and \
                    int(columns[0]) == process_group:
                rss_kib += int(columns[1])
        result[0] = max(result[0], rss_kib * 1024)
        result[1] += 1
        stop_event.wait(0.1)


def run_once(options, mode, input_path):
    time_flag = "-l" if platform.system() == "Darwin" else "-v"
    with tempfile.NamedTemporaryFile() as time_output:
        command = [
            "/usr/bin/time",
            time_flag,
            "-o",
            time_output.name,
            options.runner,
            "-ae-dense-octagon=false",
            *MODES[mode],
            "-stat=false",
            f"-extapi={options.extapi}",
            input_path,
        ]
        start = time.perf_counter()
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=True,
        )
        stop_event = threading.Event()
        sampled_peak_rss = [0, 0]
        sampler = threading.Thread(
            target=sample_process_group_rss,
            args=(process.pid, stop_event, sampled_peak_rss),
        )
        sampler.start()
        try:
            output, _ = process.communicate(timeout=options.timeout)
            status = "pass" if process.returncode == 0 else "fail"
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate()
            status = "timeout"
        stop_event.set()
        sampler.join()
        elapsed = time.perf_counter() - start
        time_output.seek(0)
        measured_peak_rss = parse_peak_rss(
            time_output.read().decode(errors="replace")
        )
        if measured_peak_rss is not None:
            peak_rss = measured_peak_rss
            rss_source = "time"
        elif sampled_peak_rss[0]:
            peak_rss = sampled_peak_rss[0]
            rss_source = "process-group-sample"
        else:
            peak_rss = None
            rss_source = ""

    match = re.search(r"AE_GENERIC_OBSERVATION analyzed_nodes=(\d+)", output)
    nodes = int(match.group(1)) if match else None
    return (status, elapsed, peak_rss, rss_source, sampled_peak_rss[0],
            sampled_peak_rss[1], nodes,
            process.returncode, output)


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
    parser.add_argument("--append", action="store_true")
    options = parser.parse_args()
    if options.repetitions <= 0 or options.timeout <= 0:
        parser.error("repetitions and timeout must be positive")

    modes = options.mode or list(MODES)
    rows = []
    for label, input_path in options.input:
        for mode in modes:
            elapsed_samples = []
            rss_samples = []
            statuses = []
            node_counts = []
            for repetition in range(1, options.repetitions + 1):
                (status, elapsed, peak_rss, rss_source, sampled_peak,
                 rss_sample_count, nodes, return_code, output) = run_once(
                     options, mode, input_path
                 )
                statuses.append(status)
                node_counts.append(nodes)
                if status != "timeout":
                    elapsed_samples.append(elapsed)
                if peak_rss is not None:
                    rss_samples.append(peak_rss)
                rows.append(
                    {
                        "input": label,
                        "mode": mode,
                        "repetition": repetition,
                        "status": status,
                        "seconds": f"{elapsed:.6f}",
                        "peak_rss_bytes": peak_rss if peak_rss is not None else "",
                        "rss_source": rss_source,
                        "sampled_peak_rss_bytes": sampled_peak,
                        "rss_samples": rss_sample_count,
                        "analyzed_nodes": nodes if nodes is not None else "",
                        "return_code": return_code,
                    }
                )
                if status == "fail":
                    diagnostic = output.rstrip().splitlines()
                    detail = diagnostic[-1] if diagnostic else "no diagnostic"
                    print(
                        f"{label}/{mode} failed rc={return_code}: {detail}",
                        flush=True,
                    )
            elapsed_summary = (
                f"time={statistics.median(elapsed_samples):.3f}s"
                if elapsed_samples else f"time>={options.timeout:.1f}s"
            )
            rss_summary = (
                f"rss={statistics.median(rss_samples) / (1024 * 1024):.1f}MiB"
                if rss_samples else "rss=n/a"
            )
            print(
                f"{label:20s} {mode:12s} {elapsed_summary} {rss_summary} "
                f"status={','.join(statuses)} nodes={node_counts[0]}",
                flush=True,
            )

    output_path = pathlib.Path(options.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    append = options.append and output_path.exists()
    with output_path.open("a" if append else "w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=FIELDNAMES)
        if not append or output_path.stat().st_size == 0:
            writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
