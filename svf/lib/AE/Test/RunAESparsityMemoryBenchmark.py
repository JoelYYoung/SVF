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
    "fun_entry",
    "directory_cow",
    "repetition",
    "status",
    "seconds",
    "peak_rss_bytes",
    "rss_source",
    "sampled_peak_rss_bytes",
    "rss_samples",
    "analyzed_nodes",
    "return_code",
    "diagnostic",
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


def sample_process_group_rss(process_group, stop_event, result,
                             memory_limit_bytes):
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
        if (memory_limit_bytes and result[0] > memory_limit_bytes and
                not result[2]):
            result[2] = True
            try:
                os.killpg(process_group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        stop_event.wait(0.1)


def run_once(options, mode, input_path, directory_kind):
    time_flag = "-l" if platform.system() == "Darwin" else "-v"
    with tempfile.NamedTemporaryFile() as time_output:
        command = [
            "/usr/bin/time",
            time_flag,
            "-o",
            time_output.name,
            options.runner,
            "-ae-dense-octagon=false",
            f"-ae-fun-entry={options.fun_entry}",
            *MODES[mode],
            f"-ae-box-directory-cow={'true' if directory_kind == 'vector' else 'false'}",
            f"-ae-box-hash-directory-cow={'true' if directory_kind == 'hash' else 'false'}",
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
        sampled_peak_rss = [0, 0, False]
        sampler = threading.Thread(
            target=sample_process_group_rss,
            args=(process.pid, stop_event, sampled_peak_rss,
                  options.memory_limit_bytes),
        )
        sampler.start()
        try:
            output, _ = process.communicate(timeout=options.timeout)
            if sampled_peak_rss[2]:
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
    diagnostic_lines = output.rstrip().splitlines()
    diagnostic = diagnostic_lines[-1] if diagnostic_lines else ""
    return (status, elapsed, peak_rss, rss_source, sampled_peak_rss[0],
            sampled_peak_rss[1], nodes,
            process.returncode, diagnostic, output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--extapi", required=True)
    parser.add_argument("--input", action="append", type=parse_input,
                        required=True, metavar="LABEL=PATH")
    parser.add_argument("--mode", action="append", choices=MODES,
                        help="default: all six modes")
    parser.add_argument("--fun-entry", choices=("main", "no-main"),
                        default="main")
    parser.add_argument("--directory-cow", action="append",
                        choices=("off", "vector", "hash"),
                        help="default: off; repeat to compare layouts")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument(
        "--memory-limit-gib", type=float, default=0.0,
        help="kill a run when sampled process-group RSS exceeds this value; "
             "0 disables the explicit limit",
    )
    parser.add_argument("--output", required=True)
    parser.add_argument("--append", action="store_true")
    options = parser.parse_args()
    if (options.repetitions <= 0 or options.timeout <= 0 or
            options.memory_limit_gib < 0):
        parser.error("repetitions/timeout must be positive and memory limit "
                     "must be non-negative")
    options.memory_limit_bytes = int(
        options.memory_limit_gib * 1024 * 1024 * 1024
    )

    modes = options.mode or list(MODES)
    directory_cow_modes = options.directory_cow or ["off"]
    rows = []
    for input_index, (label, input_path) in enumerate(options.input):
        elapsed_samples = {
            (mode, kind): []
            for mode in modes
            for kind in directory_cow_modes
        }
        rss_samples = {
            (mode, kind): []
            for mode in modes
            for kind in directory_cow_modes
        }
        statuses = {
            (mode, kind): []
            for mode in modes
            for kind in directory_cow_modes
        }
        node_counts = {
            (mode, kind): []
            for mode in modes
            for kind in directory_cow_modes
        }
        for repetition in range(1, options.repetitions + 1):
            mode_offset = ((input_index + repetition - 1) % len(modes))
            ordered_modes = modes[mode_offset:] + modes[:mode_offset]
            for mode in ordered_modes:
                mode_index = modes.index(mode)
                offset = ((input_index + mode_index + repetition - 1) %
                          len(directory_cow_modes))
                ordered_kinds = (directory_cow_modes[offset:] +
                                 directory_cow_modes[:offset])
                for cow_mode in ordered_kinds:
                    (status, elapsed, peak_rss, rss_source, sampled_peak,
                     rss_sample_count, nodes, return_code, diagnostic,
                     output) = run_once(
                         options, mode, input_path, cow_mode
                    )
                    key = (mode, cow_mode)
                    statuses[key].append(status)
                    node_counts[key].append(nodes)
                    if status == "pass":
                        elapsed_samples[key].append(elapsed)
                    if peak_rss is not None:
                        rss_samples[key].append(peak_rss)
                    rows.append(
                        {
                            "input": label,
                            "mode": mode,
                            "fun_entry": options.fun_entry,
                            "directory_cow": cow_mode,
                            "repetition": repetition,
                            "status": status,
                            "seconds": f"{elapsed:.6f}",
                            "peak_rss_bytes": (
                                peak_rss if peak_rss is not None else ""
                            ),
                            "rss_source": rss_source,
                            "sampled_peak_rss_bytes": sampled_peak,
                            "rss_samples": rss_sample_count,
                            "analyzed_nodes": nodes if nodes is not None else "",
                            "return_code": return_code,
                            "diagnostic": diagnostic,
                        }
                    )
                    if status == "fail":
                        diagnostic_lines = output.rstrip().splitlines()
                        detail = (diagnostic_lines[-1] if diagnostic_lines else
                                  "no diagnostic")
                        print(
                            f"{label}/{mode}/cow={cow_mode} failed "
                            f"rc={return_code}: {detail}", flush=True
                        )
        for mode in modes:
            for cow_mode in directory_cow_modes:
                key = (mode, cow_mode)
                elapsed_summary = (
                    f"time={statistics.median(elapsed_samples[key]):.3f}s"
                    if elapsed_samples[key] else "time=n/a"
                )
                rss_summary = (
                    f"rss={statistics.median(rss_samples[key]) / (1024 * 1024):.1f}MiB"
                    if rss_samples[key] else "rss=n/a"
                )
                print(
                    f"{label:20s} {mode:12s} cow={cow_mode:3s} "
                    f"{elapsed_summary} {rss_summary} "
                    f"status={','.join(statuses[key])} "
                    f"nodes={node_counts[key][0]}",
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
