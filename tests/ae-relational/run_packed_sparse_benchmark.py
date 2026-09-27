#!/usr/bin/env python3
"""Bounded, local numerical-kernel experiment, not a benchmark-bc campaign.

Preserves all raw attempts. No retries; one warmup and three measured attempts
per cell, alternating mode order. Mutation failure is required validation.
"""
import argparse
import csv
import hashlib
import io
import json
import pathlib
import platform
import shutil
import subprocess
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    attempts = []
    records = []

    def run(label, options, mutation=False):
        started = time.monotonic()
        command = [str(binary), *options]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            code, stdout, stderr = result.returncode, result.stdout, result.stderr
        except subprocess.TimeoutExpired as exc:
            code = "timeout"
            stdout = (exc.stdout or b"").decode() if isinstance(exc.stdout, bytes) else exc.stdout or ""
            stderr = (exc.stderr or b"").decode() if isinstance(exc.stderr, bytes) else exc.stderr or ""
        wall = time.monotonic() - started
        (args.output / (label + ".stdout")).write_text(stdout)
        (args.output / (label + ".stderr")).write_text(stderr)
        accepted = (code == 1 and "original packed equation mismatch" in stderr) if mutation else code == 0
        attempts.append(dict(label=label, command=command, exit=code, wall_s=wall,
                             accepted=accepted, purpose="mutation" if mutation else "execution"))
        (args.output / "attempts.json").write_text(json.dumps(attempts, indent=2) + "\n")
        if not accepted:
            raise RuntimeError(f"{label}: rejected, preserved raw attempt; no retry")
        return stdout, wall

    metadata = dict(binary=str(binary), binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    platform=platform.platform(), processor=platform.machine(),
                    timeout_s=60, warmups=1, measured_repetitions=3,
                    scope="synthetic fixed-pair mathematical-integer kernel; not LLVM/benchmark-bc",
                    pack_sizes=[2], counts=[8, 32, 64], widening=False)
    root = pathlib.Path(__file__).resolve().parents[2]
    metadata["base_head"] = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    metadata["sources"] = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in [
        root / "svf/include/AE/Core/PackedSparseAnalysis.h",
        root / "svf/lib/AE/Core/PackedSparseAnalysis.cpp",
        root / "tests/ae-relational/PackedSparseAnalysisTest.cpp",
        pathlib.Path(__file__).resolve(),
    ]}
    snapshot = args.output / "source-snapshot"
    for relative in metadata["sources"]:
        target = snapshot / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(root / relative, target)
    shutil.copy2(binary, args.output / "PackedSparseAnalysisTest")
    metadata["query_ledgers"] = {str(count): [
        dict(id=f"pairs-{count}/node-{3*count+3*i+3}/v{2*i+2}-le-6",
             node=3*count+3*i+3, variable=2*i+2, relation="<=", constant=6)
        for i in range(count)] for count in metadata["counts"]}
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    run("validation", [])
    run("mutation", ["--mutation"], mutation=True)
    for count in metadata["counts"]:
        for repetition in range(4):
            modes = ["dense", "sparse"] if repetition % 2 == 0 else ["sparse", "dense"]
            for mode in modes:
                stdout, wall = run(f"pairs-{count}-{mode}-r{repetition}", ["--bench", str(count), mode])
                rows = list(csv.DictReader(io.StringIO(stdout)))
                if len(rows) != 1:
                    raise RuntimeError("malformed benchmark CSV")
                record = rows[0]
                if int(record["queries"]) != count or int(record["safe"]) != count:
                    raise RuntimeError("query count mismatch")
                record.update(repetition=repetition, warmup=repetition == 0, process_wall_s=wall)
                records.append(record)
                with (args.output / "records.csv").open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(records[0]), lineterminator="\n")
                    writer.writeheader()
                    writer.writerows(records)
    summary = dict(attempts=len(attempts), accepted=sum(a["accepted"] for a in attempts),
                   cumulative_process_wall_s=sum(a["wall_s"] for a in attempts),
                   note="All checks included in total time; solve_peak sampled before verification; no real-program claim.")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary))


if __name__ == "__main__":
    main()
