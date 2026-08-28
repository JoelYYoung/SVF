#!/usr/bin/env python3

"""Measure native Box production workloads and collect opt-in telemetry.

Each observation runs in a fresh process. Profiled and unprofiled executions
are interleaved in a deterministic balanced order so telemetry overhead can be
measured without changing the analyzer's abstract semantics.
"""

import argparse
import csv
import pathlib
import re
import statistics
import subprocess
import time


MODES = {
    "dense": ["-ae-sparsity=dense", "-ae-dense-legacy-interval=false"],
    "semi": ["-ae-sparsity=semi-sparse", "-ae-sparse-legacy-interval=false"],
    "full": ["-ae-sparsity=sparse", "-ae-sparse-legacy-interval=false"],
}

TELEMETRY_PREFIXES = (
    "BOX_STORAGE_OP ",
    "BOX_STORAGE_SHAPE ",
    "BOX_STORAGE_HIST ",
)


def parse_input(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError("input must be LABEL=PATH")
    label, path = value.split("=", 1)
    if not label or not pathlib.Path(path).is_file():
        raise argparse.ArgumentTypeError(f"invalid benchmark input: {value}")
    return label, path


def parse_fields(line):
    fields = {}
    for token in line.split()[1:]:
        key, value = token.split("=", 1)
        fields[key] = value
    return fields


def run_once(options, mode, input_path, profile, directory_kind):
    command = [
        options.runner,
        "-ae-dense-octagon=false",
        *MODES[mode],
        f"-ae-box-directory-cow={'true' if directory_kind == 'vector' else 'false'}",
        f"-ae-box-hash-directory-cow={'true' if directory_kind == 'hash' else 'false'}",
        f"-ae-box-storage-profile={'true' if profile else 'false'}",
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
        return_code = result.returncode
        output = result.stdout
    except subprocess.TimeoutExpired as error:
        elapsed = time.perf_counter() - start
        status = "timeout"
        return_code = ""
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")

    match = re.search(r"AE_GENERIC_OBSERVATION analyzed_nodes=(\d+)", output)
    nodes = int(match.group(1)) if match else ""
    telemetry = []
    if profile:
        for line in output.splitlines():
            if line.startswith(TELEMETRY_PREFIXES):
                record = {"record": line.split(maxsplit=1)[0]}
                record.update(parse_fields(line))
                telemetry.append(record)
    return status, elapsed, return_code, nodes, telemetry, output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--extapi", required=True)
    parser.add_argument("--input", action="append", type=parse_input,
                        required=True, metavar="LABEL=PATH")
    parser.add_argument("--mode", action="append", choices=MODES,
                        help="default: dense, semi, and full")
    parser.add_argument("--directory-cow", action="append",
                        choices=("off", "vector", "hash"),
                        help="default: off; repeat to compare layouts")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--runs-output", required=True)
    parser.add_argument("--telemetry-output", required=True)
    options = parser.parse_args()
    if options.repetitions <= 0 or options.timeout <= 0:
        parser.error("repetitions and timeout must be positive")

    run_rows = []
    telemetry_rows = []
    modes = options.mode or list(MODES)
    directory_cow_modes = options.directory_cow or ["off"]
    for input_index, (label, input_path) in enumerate(options.input):
        for mode_index, mode in enumerate(modes):
            elapsed_samples = {
                (cow_mode, profile): []
                for cow_mode in directory_cow_modes
                for profile in (False, True)
            }
            node_counts = {
                (cow_mode, profile): set()
                for cow_mode in directory_cow_modes
                for profile in (False, True)
            }
            cases = [
                (cow_mode, profile)
                for cow_mode in directory_cow_modes
                for profile in (False, True)
            ]
            for repetition in range(1, options.repetitions + 1):
                offset = (input_index + mode_index + repetition) % len(cases)
                ordered_cases = cases[offset:] + cases[:offset]
                for cow_mode, profile in ordered_cases:
                    status, elapsed, return_code, nodes, telemetry, output = (
                        run_once(options, mode, input_path, profile,
                                 cow_mode)
                    )
                    run_rows.append(
                        {
                            "input": label,
                            "path": input_path,
                            "mode": mode,
                            "directory_cow": cow_mode,
                            "repetition": repetition,
                            "profile": "enabled" if profile else "disabled",
                            "status": status,
                            "seconds": f"{elapsed:.6f}",
                            "analyzed_nodes": nodes,
                            "return_code": return_code,
                        }
                    )
                    key = (cow_mode, profile)
                    if status == "pass":
                        elapsed_samples[key].append(elapsed)
                        node_counts[key].add(nodes)
                    elif status == "fail":
                        raise RuntimeError(
                            f"{label}/{mode}/cow={cow_mode}/"
                            f"profile={profile} failed:\n{output.rstrip()}"
                        )
                    for record in telemetry:
                        telemetry_rows.append(
                            {
                                "input": label,
                                "mode": mode,
                                "directory_cow": cow_mode,
                                "repetition": repetition,
                                **record,
                            }
                        )

            for cow_mode in directory_cow_modes:
                baseline = elapsed_samples[(cow_mode, False)]
                profiled = elapsed_samples[(cow_mode, True)]
                if baseline and profiled:
                    baseline_median = statistics.median(baseline)
                    profiled_median = statistics.median(profiled)
                    overhead = 100.0 * (profiled_median / baseline_median - 1.0)
                    summary = (
                        f"off={baseline_median:.3f}s on={profiled_median:.3f}s "
                        f"overhead={overhead:+.1f}%"
                    )
                else:
                    summary = f"incomplete (timeout={options.timeout:.1f}s)"
                print(
                    f"{label:24s} {mode:5s} cow={cow_mode:3s} {summary} "
                    f"nodes_off={sorted(node_counts[(cow_mode, False)])} "
                    f"nodes_on={sorted(node_counts[(cow_mode, True)])}"
                )

    runs_path = pathlib.Path(options.runs_output)
    runs_path.parent.mkdir(parents=True, exist_ok=True)
    with runs_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=run_rows[0].keys())
        writer.writeheader()
        writer.writerows(run_rows)

    telemetry_path = pathlib.Path(options.telemetry_output)
    telemetry_path.parent.mkdir(parents=True, exist_ok=True)
    telemetry_fields = [
        "input", "mode", "directory_cow", "repetition", "record"
    ]
    telemetry_fields.extend(
        sorted({key for row in telemetry_rows for key in row}
               - set(telemetry_fields))
    )
    with telemetry_path.open("w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=telemetry_fields)
        writer.writeheader()
        writer.writerows(telemetry_rows)


if __name__ == "__main__":
    main()
