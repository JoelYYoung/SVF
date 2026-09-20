#!/usr/bin/env python3
"""Run one balanced Whole-versus-Chunk Box storage performance case."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def single_value(pattern, text):
    values = re.findall(pattern, text, re.M)
    return values[-1] if values else None


def run_variant(executable, extapi, bitcode, cap_seconds, output):
    output.mkdir()
    command = [
        str(executable),
        "-ae-sparsity=semi-sparse",
        "-ae-fun-entry=main",
        "-stat=true",
        f"-extapi={extapi}",
        str(bitcode),
    ]
    log = output / "analysis.log"
    timing = output / "time.txt"
    wrapped = [
        "/usr/bin/time", "-v", "-o", str(timing),
        "timeout", "--signal=TERM", str(cap_seconds), *command,
    ]
    with log.open("w") as stream:
        completed = subprocess.run(wrapped, stdout=stream,
                                   stderr=subprocess.STDOUT)
    log_text = log.read_text(errors="replace")
    time_text = timing.read_text(errors="replace") if timing.exists() else ""
    record = {
        "command": command,
        "exit_code": completed.returncode,
        "completed": completed.returncode == 0,
        "wall_clock": single_value(
            r"^\s*Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*(.+)$",
            time_text,
        ),
        "max_rss_kib": single_value(
            r"^\s*Maximum resident set size \(kbytes\):\s*(\d+)$",
            time_text,
        ),
        "ae_seconds": single_value(r"^Total_Time\(sec\)\s+(.+)$", log_text),
        "function_coverage_percent": single_value(
            r"^Func_Coverage_Percent\s+(.+)$", log_text
        ),
        "icfg_node_trace": single_value(r"^ICFG_Node_Trace\s+(.+)$", log_text),
    }
    (output / "record.json").write_text(json.dumps(record, indent=2) + "\n")
    if not record["completed"]:
        raise RuntimeError(
            f"analysis failed with exit {completed.returncode}: {output}"
        )
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--study", required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--bitcode", required=True)
    parser.add_argument("--input-sha256", required=True)
    parser.add_argument("--cap-seconds", type=int, required=True)
    parser.add_argument("--result-set", required=True)
    parser.add_argument("--first", choices=("whole", "chunk"), required=True)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()

    study = Path(args.study)
    bitcode = Path(args.bitcode)
    output = study / "results" / args.result_set / args.program
    if output.exists():
        raise RuntimeError(f"result directory already exists: {output}")
    output.mkdir(parents=True)
    if digest(bitcode) != args.input_sha256:
        raise RuntimeError(f"input changed: {args.program}")

    builds = {
        "whole": study / "build-directory-cow-off",
        "chunk": study / "build-directory-chunk-off",
    }
    executables = {name: build / "bin/ae" for name, build in builds.items()}
    extapi = study / "build-original/lib/extapi.bc"
    runtime_names = (
        "libAbstractDomainCore.so.3.4",
        "libSvfCore.so.3.4",
        "libSvfLLVM.so.3.4",
    )
    manifest = {
        "schema": 1,
        "program": args.program,
        "input": str(bitcode),
        "input_sha256": digest(bitcode),
        "extapi": str(extapi),
        "extapi_sha256": digest(extapi),
        "first": args.first,
        "repeats": args.repeats,
        "variants": {},
    }
    for name, executable in executables.items():
        manifest["variants"][name] = {
            "executable": str(executable),
            "executable_sha256": digest(executable),
            "runtime_libraries": {
                library: digest(builds[name] / "lib" / library)
                for library in runtime_names
            },
        }

    second = "chunk" if args.first == "whole" else "whole"
    runs = []
    for name in (args.first, second):
        record = run_variant(
            executables[name], extapi, bitcode, args.cap_seconds,
            output / f"warmup-{name}",
        )
        record.update({"variant": name, "phase": "warmup", "repeat": None})
        runs.append(record)

    for repeat in range(args.repeats):
        order = (args.first, second) if repeat % 2 == 0 else (second, args.first)
        for name in order:
            record = run_variant(
                executables[name], extapi, bitcode, args.cap_seconds,
                output / f"measured-{repeat}-{name}",
            )
            record.update({"variant": name, "phase": "measured", "repeat": repeat})
            runs.append(record)

    measured = [record for record in runs if record["phase"] == "measured"]
    for field in ("function_coverage_percent", "icfg_node_trace"):
        values = {record[field] for record in measured}
        if len(values) != 1:
            raise RuntimeError(f"measured workload mismatch for {field}: {values}")
    manifest["runs"] = runs
    result = output / "result.json"
    result.write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "finished").touch()
    print(
        "T5_STORAGE_PERF_RESULT",
        f"program={args.program}",
        f"first={args.first}",
        f"measured_runs={len(measured)}",
        f"coverage={measured[0]['function_coverage_percent']}",
        f"icfg_trace={measured[0]['icfg_node_trace']}",
        f"result_sha256={digest(result)}",
    )


if __name__ == "__main__":
    main()
