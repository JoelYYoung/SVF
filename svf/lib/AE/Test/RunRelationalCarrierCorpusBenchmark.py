#!/usr/bin/env python3
"""Run production relational abstract interpretation on a real corpus.

Each candidate runs in a fresh process. Candidate order rotates, failures stay
in the raw table, full logs are retained, and Octagon carriers expose a common
semantic checksum for differential correctness checking.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import os
import pathlib
import platform
import re
import shlex
import signal
import subprocess
import tempfile
import threading
import time


CANDIDATES = {
    "box": {"domain": "box", "carrier": "native-box", "storage": ""},
    "octagon-dense-half": {
        "domain": "octagon", "carrier": "dense-half", "storage": "dense-half",
    },
    "octagon-sparse-finite": {
        "domain": "octagon", "carrier": "sparse-finite", "storage": "sparse-finite",
    },
    "octagon-component-dense": {
        "domain": "octagon", "carrier": "component-dense", "storage": "component-dense",
    },
    "polyhedra-native-hv": {
        "domain": "polyhedra", "carrier": "native-hv-dense", "storage": "",
    },
}
FIELDNAMES = [
    "run_id", "timestamp_utc", "host", "runner_commit", "runner_sha256",
    "extapi_sha256", "manifest_version", "manifest_sha256", "input", "path",
    "bitcode_sha256", "lane", "scale",
    "program_family", "candidate", "domain", "carrier", "repetition",
    "order_position", "status", "seconds", "user_seconds", "sys_seconds",
    "peak_rss_bytes", "rss_source", "sampled_peak_rss_bytes", "rss_samples",
    "selected_dimensions", "dimension_limit", "octagon_selected",
    "polyhedra_selected", "analyzed_nodes", "semantic_states",
    "semantic_checksum", "semantic_checksum_enabled", "return_code", "timeout_seconds",
    "memory_limit_bytes", "telemetry_path", "log_path", "command", "diagnostic",
]


def sha256(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def parse_input(value: str) -> dict[str, str | pathlib.Path]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("input must be LABEL=PATH")
    label, raw_path = value.split("=", 1)
    path = pathlib.Path(raw_path)
    if not label or not path.is_file():
        raise argparse.ArgumentTypeError(f"invalid benchmark input: {value}")
    return {
        "label": label, "path": path.resolve(), "bitcode_sha256": sha256(path),
        "lane": "", "scale": "", "program_family": "",
    }


def load_manifest(path: pathlib.Path, corpus_root: pathlib.Path,
                  scales: set[str]) -> tuple[list[dict[str, str | pathlib.Path]], str]:
    records: list[dict[str, str | pathlib.Path]] = []
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream, delimiter="\t"):
            if scales and row["scale"] not in scales:
                continue
            bitcode = corpus_root / row["bitcode_path"]
            if not bitcode.is_file():
                raise ValueError(f"manifest bitcode is missing: {bitcode}")
            if sha256(bitcode) != row["sha256"]:
                raise ValueError(f"manifest SHA-256 mismatch: {bitcode}")
            records.append({
                "label": row["benchmark_id"], "path": bitcode.resolve(),
                "bitcode_sha256": row["sha256"], "lane": row["lane"],
                "scale": row["scale"], "program_family": row["program_family"],
            })
    return records, sha256(path)


def parse_peak_rss(text: str) -> int | None:
    if platform.system() == "Darwin":
        match = re.search(r"^\s*(\d+)\s+maximum resident set size$", text, re.MULTILINE)
        return int(match.group(1)) if match else None
    match = re.search(
        r"^\s*Maximum resident set size \(kbytes\):\s*(\d+)$", text, re.MULTILINE
    )
    return int(match.group(1)) * 1024 if match else None


def parse_cpu_times(text: str) -> tuple[str, str]:
    if platform.system() == "Darwin":
        user = re.search(r"^\s*([0-9.]+)\s+user$", text, re.MULTILINE)
        system = re.search(r"^\s*([0-9.]+)\s+system$", text, re.MULTILINE)
    else:
        user = re.search(r"^\s*User time \(seconds\):\s*([0-9.]+)$", text, re.MULTILINE)
        system = re.search(r"^\s*System time \(seconds\):\s*([0-9.]+)$", text, re.MULTILINE)
    return user.group(1) if user else "", system.group(1) if system else ""


def sample_process_group_rss(process_group: int, stop_event: threading.Event,
                             result: list[int | bool], memory_limit_bytes: int) -> None:
    while not stop_event.is_set():
        sample = subprocess.run(
            ["ps", "-axo", "pgid=,rss="], stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, check=False,
        )
        rss_kib = sum(
            int(columns[1]) for columns in map(str.split, sample.stdout.splitlines())
            if len(columns) == 2 and columns[0].isdigit() and int(columns[0]) == process_group
        )
        result[0] = max(int(result[0]), rss_kib * 1024)
        result[1] = int(result[1]) + 1
        if memory_limit_bytes and int(result[0]) > memory_limit_bytes and not result[2]:
            result[2] = True
            try:
                os.killpg(process_group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        stop_event.wait(0.1)


def parse_selection(output: str, domain: str) -> tuple[int | None, int | None, int | None]:
    prefix = "OCTAGON" if domain == "octagon" else "POLYHEDRA"
    match = re.search(
        rf"{prefix}_SELECTION\s+.*dimensions=(\d+)\s+limit=(\d+)\s+selected=(\d+)", output
    )
    return ((int(match.group(1)), int(match.group(2)), int(match.group(3)))
            if match else (None, None, None))


def probe_dimensions(options: argparse.Namespace, input_path: pathlib.Path,
                     domain: str) -> tuple[int | None, int | None, int | None, str]:
    environment = os.environ.copy()
    environment.pop("SVF_OCTAGON_TELEMETRY", None)
    environment.pop("SVF_POLYHEDRA_TELEMETRY", None)
    if not options.disable_operation_telemetry:
        environment["SVF_OCTAGON_TELEMETRY" if domain == "octagon"
                    else "SVF_POLYHEDRA_TELEMETRY"] = "stderr"
    environment["SVF_RELATIONAL_SELECTION_ONLY"] = "1"
    command = [
        options.runner, "-ae-sparsity=dense", "-ae-dense-legacy-interval=false",
        f"-ae-dense-octagon={'true' if domain == 'octagon' else 'false'}",
        f"-ae-dense-octagon-max-dimensions={options.dimension_limit}",
        "-ae-octagon-storage=dense-half",
        f"-ae-dense-polyhedra={'true' if domain == 'polyhedra' else 'false'}",
        f"-ae-dense-polyhedra-max-dimensions={options.dimension_limit}",
        f"-ae-fun-entry={options.fun_entry}", "-stat=false",
        f"-extapi={options.extapi}", str(input_path),
    ]
    try:
        result = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            timeout=options.probe_timeout, check=False, env=environment,
        )
    except subprocess.TimeoutExpired:
        return None, options.dimension_limit, None, "dimension probe timed out"
    if result.returncode != 0:
        lines = result.stdout.rstrip().splitlines()
        return None, options.dimension_limit, None, (
            lines[-1] if lines else f"dimension probe exited {result.returncode}"
        )
    dimensions, limit, selected = parse_selection(result.stdout, domain)
    return ((dimensions, limit, selected, "") if dimensions is not None else
            (None, options.dimension_limit, None, "dimension observation missing"))


def base_row(options: argparse.Namespace, benchmark: dict[str, str | pathlib.Path],
             candidate: str, repetition: int, order_position: int) -> dict[str, object]:
    config = CANDIDATES[candidate]
    label = str(benchmark["label"])
    row = {field: "" for field in FIELDNAMES}
    row.update({
        "run_id": f"{options.manifest_sha256[:12]}:{label}:{candidate}:r{repetition}",
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "host": platform.node(), "runner_commit": options.runner_commit,
        "runner_sha256": options.runner_sha256,
        "extapi_sha256": options.extapi_sha256,
        "manifest_version": options.manifest_version,
        "manifest_sha256": options.manifest_sha256, "input": label,
        "path": str(benchmark["path"]), "bitcode_sha256": benchmark["bitcode_sha256"],
        "lane": benchmark["lane"], "scale": benchmark["scale"],
        "program_family": benchmark["program_family"], "candidate": candidate,
        "domain": config["domain"], "carrier": config["carrier"],
        "repetition": repetition, "order_position": order_position,
        "dimension_limit": options.dimension_limit, "timeout_seconds": options.timeout,
        "memory_limit_bytes": options.memory_limit_bytes,
        "semantic_checksum_enabled": int(options.semantic_checksum),
    })
    return row


def run_once(options: argparse.Namespace, benchmark: dict[str, str | pathlib.Path],
             candidate: str, repetition: int, order_position: int,
             selection: tuple[int | None, int | None, int | None, str] | None) -> dict[str, object]:
    row = base_row(options, benchmark, candidate, repetition, order_position)
    config = CANDIDATES[candidate]
    domain = config["domain"]
    if selection is not None:
        dimensions, limit, selected, probe_diagnostic = selection
        row["selected_dimensions"] = dimensions if dimensions is not None else ""
        row["dimension_limit"] = limit if limit is not None else options.dimension_limit
        row["octagon_selected" if domain == "octagon" else "polyhedra_selected"] = (
            selected if selected is not None else ""
        )
        if probe_diagnostic:
            row.update(status="probe-fail", diagnostic=probe_diagnostic)
            return row
        if selected != 1:
            row.update(status="dimension-limit",
                       diagnostic=f"{domain} not selected at configured dimension limit")
            return row

    label = str(benchmark["label"])
    environment = os.environ.copy()
    telemetry_path = ""
    if domain == "octagon" and not options.disable_operation_telemetry:
        telemetry_path = str(options.telemetry_directory /
                             f"{label}-{candidate}-r{repetition}.csv")
        environment["SVF_OCTAGON_TELEMETRY"] = telemetry_path
    else:
        environment.pop("SVF_OCTAGON_TELEMETRY", None)
    if domain == "polyhedra" and not options.disable_operation_telemetry:
        environment["SVF_POLYHEDRA_TELEMETRY"] = "stderr"
    else:
        environment.pop("SVF_POLYHEDRA_TELEMETRY", None)
    if options.semantic_checksum:
        environment["SVF_RELATIONAL_SEMANTIC_CHECKSUM"] = "1"
    else:
        environment.pop("SVF_RELATIONAL_SEMANTIC_CHECKSUM", None)

    octagon = domain == "octagon"
    polyhedra = domain == "polyhedra"
    time_flag = "-l" if platform.system() == "Darwin" else "-v"
    log_path = options.log_directory / f"{label}-{candidate}-r{repetition}.log"
    with tempfile.NamedTemporaryFile() as time_output:
        command = [
            "/usr/bin/time", time_flag, "-o", time_output.name, options.runner,
            "-ae-sparsity=dense", "-ae-dense-legacy-interval=false",
            f"-ae-dense-octagon={'true' if octagon else 'false'}",
            f"-ae-dense-octagon-max-dimensions={options.dimension_limit}",
            f"-ae-octagon-storage={config['storage'] or 'dense-half'}",
            f"-ae-dense-polyhedra={'true' if polyhedra else 'false'}",
            f"-ae-dense-polyhedra-max-dimensions={options.dimension_limit}",
            "-ae-box-directory-cow=false", "-ae-box-hash-directory-cow=false",
            f"-ae-fun-entry={options.fun_entry}", "-stat=false",
            f"-extapi={options.extapi}", str(benchmark["path"]),
        ]
        row["command"] = shlex.join(command)
        start = time.perf_counter()
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            start_new_session=True, env=environment,
        )
        stop_event = threading.Event()
        sampled_peak: list[int | bool] = [0, 0, False]
        sampler = threading.Thread(
            target=sample_process_group_rss,
            args=(process.pid, stop_event, sampled_peak, options.memory_limit_bytes),
        )
        sampler.start()
        try:
            output, _ = process.communicate(timeout=options.timeout)
            status = ("memory-limit" if sampled_peak[2] else
                      ("pass" if process.returncode == 0 else "fail"))
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            output, _ = process.communicate()
            status = "timeout"
        stop_event.set()
        sampler.join()
        elapsed = time.perf_counter() - start
        time_output.seek(0)
        time_text = time_output.read().decode(errors="replace")
        measured_peak = parse_peak_rss(time_text)
        user_seconds, sys_seconds = parse_cpu_times(time_text)

    log_path.write_text(output, encoding="utf-8", errors="replace")
    peak_rss = measured_peak if measured_peak is not None else int(sampled_peak[0])
    dimensions, dimension_limit, selected = (None, None, None)
    if domain in {"octagon", "polyhedra"}:
        dimensions, dimension_limit, selected = parse_selection(output, domain)
    if selection is not None and dimensions is None:
        dimensions, dimension_limit, selected, _ = selection
    nodes = re.search(r"AE_GENERIC_OBSERVATION analyzed_nodes=(\d+)", output)
    checksum = re.search(
        rf"AE_NUMERICAL_CHECKSUM domain={domain} states=(\d+) checksum=([0-9a-f]+)", output
    )
    if status == "pass" and (
        nodes is None or (options.semantic_checksum and checksum is None)
    ):
        status = "observation-missing"
    lines = output.rstrip().splitlines()
    row.update({
        "status": status, "seconds": f"{elapsed:.6f}",
        "user_seconds": user_seconds, "sys_seconds": sys_seconds,
        "peak_rss_bytes": peak_rss or "",
        "rss_source": "time" if measured_peak is not None else
                      ("process-group-sample" if sampled_peak[0] else ""),
        "sampled_peak_rss_bytes": sampled_peak[0], "rss_samples": sampled_peak[1],
        "selected_dimensions": dimensions if dimensions is not None else "",
        "dimension_limit": dimension_limit if dimension_limit is not None else options.dimension_limit,
        "octagon_selected": selected if octagon and selected is not None else "",
        "polyhedra_selected": selected if polyhedra and selected is not None else "",
        "analyzed_nodes": int(nodes.group(1)) if nodes else "",
        "semantic_states": int(checksum.group(1)) if checksum else "",
        "semantic_checksum": checksum.group(2) if checksum else "",
        "return_code": process.returncode, "telemetry_path": telemetry_path,
        "log_path": str(log_path), "diagnostic": lines[-1] if lines else "",
    })
    return row


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--extapi", required=True)
    parser.add_argument("--input", action="append", type=parse_input, metavar="LABEL=PATH")
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--corpus-root", type=pathlib.Path)
    parser.add_argument("--scale", action="append", choices=("xs", "s", "m", "l", "xl"))
    parser.add_argument("--candidate", action="append", choices=tuple(CANDIDATES))
    parser.add_argument("--fun-entry", choices=("main", "no-main"), default="main")
    parser.add_argument("--dimension-limit", type=int, default=4096)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--probe-timeout", type=float, default=120.0)
    parser.add_argument("--memory-limit-gib", type=float, default=24.0)
    parser.add_argument("--shard-index", type=int, default=0)
    parser.add_argument("--shard-count", type=int, default=1)
    parser.add_argument("--runner-commit", default="unknown")
    parser.add_argument("--manifest-version")
    parser.add_argument("--semantic-checksum", action="store_true")
    parser.add_argument(
        "--disable-operation-telemetry", action="store_true",
        help="Disable Octagon/Polyhedra operation profiling for clean end-to-end timing.",
    )
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--telemetry-directory", type=pathlib.Path, required=True)
    parser.add_argument("--log-directory", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    options = parser.parse_args()
    if not options.input and not options.manifest:
        parser.error("at least one --input or --manifest is required")
    if options.manifest and options.input:
        parser.error("--input and --manifest are mutually exclusive")
    if options.manifest and not options.manifest.is_file():
        parser.error("--manifest must name an existing file")
    if options.shard_count <= 0 or not 0 <= options.shard_index < options.shard_count:
        parser.error("shard index must be in [0, shard count)")
    if (options.dimension_limit <= 0 or options.repetitions <= 0 or
            options.timeout <= 0 or options.probe_timeout <= 0 or
            options.memory_limit_gib < 0):
        parser.error("limits and repetitions must be positive")

    options.runner = str(pathlib.Path(options.runner).resolve())
    options.extapi = str(pathlib.Path(options.extapi).resolve())
    if not pathlib.Path(options.runner).is_file():
        parser.error("--runner must name an existing file")
    if not pathlib.Path(options.extapi).is_file():
        parser.error("--extapi must name an existing file")
    options.runner_sha256 = sha256(pathlib.Path(options.runner))
    options.extapi_sha256 = sha256(pathlib.Path(options.extapi))
    options.memory_limit_bytes = int(options.memory_limit_gib * 1024**3)
    options.telemetry_directory.mkdir(parents=True, exist_ok=True)
    options.log_directory = options.log_directory or options.output.parent / "logs"
    options.log_directory.mkdir(parents=True, exist_ok=True)
    candidates = options.candidate or list(CANDIDATES)

    if options.manifest:
        corpus_root = (options.corpus_root or options.manifest.parent.parent).resolve()
        benchmarks, options.manifest_sha256 = load_manifest(
            options.manifest.resolve(), corpus_root, set(options.scale or ())
        )
        options.manifest_version = options.manifest_version or options.manifest.name
    else:
        benchmarks = options.input
        options.manifest_sha256 = hashlib.sha256(
            "\n".join(str(row["bitcode_sha256"]) for row in benchmarks).encode()
        ).hexdigest()
        options.manifest_version = options.manifest_version or "explicit-inputs"
    benchmarks = [benchmark for index, benchmark in enumerate(benchmarks)
                  if index % options.shard_count == options.shard_index]

    completed: set[tuple[str, str, int]] = set()
    append = options.resume and options.output.is_file()
    if append:
        with options.output.open(newline="") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames != FIELDNAMES:
                parser.error(
                    "--resume output schema does not match this runner version"
                )
            completed = {
                (row["input"], row["candidate"], int(row["repetition"]))
                for row in reader
            }
    options.output.parent.mkdir(parents=True, exist_ok=True)
    with options.output.open("a" if append else "w", newline="") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=FIELDNAMES)
        if not append:
            writer.writeheader()
            output_file.flush()

        domains = {CANDIDATES[candidate]["domain"] for candidate in candidates} - {"box"}
        selections = {
            (str(benchmark["label"]), domain): probe_dimensions(
                options, pathlib.Path(benchmark["path"]), domain
            )
            for benchmark in benchmarks for domain in domains
        }
        for input_index, benchmark in enumerate(benchmarks):
            label = str(benchmark["label"])
            for repetition in range(1, options.repetitions + 1):
                offset = (input_index + repetition - 1) % len(candidates)
                order = candidates[offset:] + candidates[:offset]
                for order_position, candidate in enumerate(order, start=1):
                    key = (label, candidate, repetition)
                    if key in completed:
                        continue
                    domain = CANDIDATES[candidate]["domain"]
                    row = run_once(
                        options, benchmark, candidate, repetition, order_position,
                        selections.get((label, domain)),
                    )
                    writer.writerow(row)
                    output_file.flush()
                    rss_mib = float(row["peak_rss_bytes"] or 0) / (1024 * 1024)
                    print(
                        f"{label:28.28s} {candidate:26s} r={repetition} "
                        f"status={str(row['status']):19s} time={row['seconds'] or '-'}s "
                        f"rss={rss_mib:.1f}MiB dims={row['selected_dimensions'] or '-'}",
                        flush=True,
                    )


if __name__ == "__main__":
    main()
