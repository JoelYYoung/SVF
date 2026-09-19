#!/usr/bin/env python3
"""Run one frozen T5.1 semantic gate case.

The storage candidates must match exactly.  Original+PR1887 may retain only
the documented numeric Bottom-to-Top compatibility residual; all key-set,
reachability, address, and other numeric differences fail the gate.  Pure
upstream is recorded as a reference because PR1887 changes its workload.
"""

import argparse
import collections
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


def projection(log):
    records = {}
    counts = collections.Counter()
    for line in log.read_text(errors="replace").splitlines():
        fields = line.split("\t")
        if not fields or fields[0] not in ("REACH", "IDENT", "VALUE"):
            continue
        key = tuple(fields[:-1])
        value = fields[-1]
        if fields[0] == "VALUE":
            value = value.replace(" ", "").replace("oo", "inf")
        if key in records and records[key] != value:
            raise RuntimeError(f"ambiguous canonical key: {key}")
        records[key] = value
        counts[fields[0]] += 1
    if not records:
        raise RuntimeError(f"empty canonical projection: {log}")
    return records, dict(counts)


def differences(left, right):
    return [
        {"key": list(key), "left": left.get(key), "right": right.get(key)}
        for key in sorted(left.keys() | right.keys())
        if left.get(key) != right.get(key)
    ]


def category(difference):
    key = difference["key"]
    if key[0] == "VALUE" and len(key) > 3:
        return f"VALUE/{key[3]}"
    return key[0]


def allowed_pr1887_residual(difference):
    return (
        category(difference) in ("VALUE/integer", "VALUE/memory-numeric")
        and difference["left"] == "BOTTOM"
        and difference["right"] == "TOP"
    )


def run_variant(executable, extapi, bitcode, cap, directory):
    directory.mkdir()
    log = directory / "analysis.log"
    timing = directory / "time.txt"
    command = [
        str(executable),
        "-ae-sparsity=semi-sparse",
        "-ae-fun-entry=main",
        "-stat=true",
        f"-extapi={extapi}",
        str(bitcode),
    ]
    wrapped = [
        "/usr/bin/time", "-v", "-o", str(timing),
        "timeout", "--signal=TERM", str(cap), *command,
    ]
    with log.open("w") as stream:
        completed = subprocess.run(wrapped, stdout=stream,
                                   stderr=subprocess.STDOUT)
    log_text = log.read_text(errors="replace")
    time_text = timing.read_text(errors="replace") if timing.exists() else ""
    records = None
    counts = {}
    projection_sha256 = None
    if completed.returncode == 0:
        records, counts = projection(log)
        projection_sha256 = hashlib.sha256(
            json.dumps(sorted(records.items())).encode()
        ).hexdigest()
    record = {
        "command": command,
        "exit_code": completed.returncode,
        "completed": completed.returncode == 0,
        "executable_sha256": digest(executable),
        "projection_sha256": projection_sha256,
        "counts": counts,
        "function_coverage_percent": re.findall(
            r"^Func_Coverage_Percent\s+(.+)$", log_text, re.M
        ),
        "icfg_node_trace": re.findall(
            r"^ICFG_Node_Trace\s+(.+)$", log_text, re.M
        ),
        "wall_clock": re.findall(
            r"^\s*Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*(.+)$",
            time_text, re.M
        ),
        "max_rss_kib": re.findall(
            r"^\s*Maximum resident set size \(kbytes\):\s*(.+)$",
            time_text, re.M
        ),
    }
    return records, record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--study", required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--bitcode", required=True)
    parser.add_argument("--input-sha256", required=True)
    parser.add_argument("--cap-seconds", type=int, required=True)
    parser.add_argument("--result-set", required=True)
    parser.add_argument("--observer-bundle", default="t5-observers-v6")
    args = parser.parse_args()

    study = Path(args.study)
    bitcode = Path(args.bitcode)
    observers = study / args.observer_bundle
    extapi = study / "build-original/lib/extapi.bc"
    output = study / "results" / args.result_set / args.program
    if output.exists():
        raise RuntimeError(f"result directory already exists: {output}")
    output.mkdir(parents=True)
    if digest(bitcode) != args.input_sha256:
        raise RuntimeError(f"input changed: {args.program}")

    candidates = ("whole", "chunk")
    runs = collections.defaultdict(list)
    run_records = []
    for repeat in range(2):
        order = candidates if repeat == 0 else tuple(reversed(candidates))
        for variant in order:
            records, record = run_variant(
                observers / variant, extapi, bitcode, args.cap_seconds,
                output / f"{variant}-{repeat}",
            )
            runs[variant].append(records)
            record.update({"variant": variant, "repeat": repeat})
            run_records.append(record)
    if any(item is None for variant in candidates for item in runs[variant]):
        raise RuntimeError("a storage candidate observer failed")

    for repeat in range(2):
        if repeat and runs["original-pr1887"][0] is None:
            break
        records, record = run_variant(
            observers / "original-pr1887", extapi, bitcode,
            args.cap_seconds, output / f"original-pr1887-{repeat}",
        )
        runs["original-pr1887"].append(records)
        record.update({"variant": "original-pr1887", "repeat": repeat})
        run_records.append(record)

    upstream, upstream_record = run_variant(
        observers / "original-upstream", extapi, bitcode, args.cap_seconds,
        output / "original-upstream-0",
    )
    upstream_record.update({"variant": "original-upstream", "repeat": 0})
    run_records.append(upstream_record)

    candidate_repeat_stable = {
        variant: runs[variant][0] == runs[variant][1]
        for variant in candidates
    }
    pr1887_completed = (
        len(runs["original-pr1887"]) == 2
        and all(item is not None for item in runs["original-pr1887"])
    )
    pr1887_repeat_stable = (
        runs["original-pr1887"][0] == runs["original-pr1887"][1]
        if pr1887_completed else None
    )
    whole_chunk = differences(runs["whole"][0], runs["chunk"][0])
    pr1887_whole = (differences(
        runs["original-pr1887"][0], runs["whole"][0]
    ) if pr1887_completed else None)
    disallowed = [
        item for item in pr1887_whole
        if not allowed_pr1887_residual(item)
    ] if pr1887_whole is not None else None
    upstream_pr1887 = (differences(
        upstream, runs["original-pr1887"][0]
    ) if upstream is not None and pr1887_completed else None)
    result = {
        "schema": 1,
        "program": args.program,
        "input": str(bitcode),
        "input_sha256": args.input_sha256,
        "common_extapi": str(extapi),
        "common_extapi_sha256": digest(extapi),
        "observer_bundle_metadata": json.loads(
            (observers / "metadata.json").read_text()
        ),
        "runs": run_records,
        "candidate_repeat_stable": candidate_repeat_stable,
        "whole_chunk_exact": not whole_chunk,
        "whole_chunk_differences": whole_chunk,
        "pr1887_completed": pr1887_completed,
        "pr1887_repeat_stable": pr1887_repeat_stable,
        "pr1887_whole_strict_exact": (
            not pr1887_whole if pr1887_whole is not None else None
        ),
        "pr1887_whole_compatible": (
            not disallowed if disallowed is not None else None
        ),
        "pr1887_whole_difference_categories": (dict(collections.Counter(
            category(item) for item in pr1887_whole
        )) if pr1887_whole is not None else None),
        "pr1887_whole_differences": pr1887_whole,
        "pr1887_whole_disallowed_differences": disallowed,
        "upstream_completed": upstream is not None,
        "upstream_pr1887_difference_categories": (dict(collections.Counter(
            category(item) for item in upstream_pr1887
        )) if upstream_pr1887 is not None else None),
        "upstream_pr1887_difference_count": (
            len(upstream_pr1887) if upstream_pr1887 is not None else None
        ),
        "upstream_pr1887_differences": upstream_pr1887,
    }
    result_file = output / "result.json"
    result_file.write_text(json.dumps(result, indent=2) + "\n")
    (output / "storage-finished").touch()
    if not all(candidate_repeat_stable.values()):
        raise RuntimeError("candidate semantic projection was not repeat-stable")
    if whole_chunk:
        raise RuntimeError("whole and chunk storage candidates differ")
    (output / "finished").touch()
    print(
        "T5_SEMANTIC_RESULT",
        f"program={args.program}",
        "candidate_repeat_stable=true",
        "whole_chunk_exact=true",
        f"pr1887_completed={str(pr1887_completed).lower()}",
        "pr1887_whole_strict_differences=" +
        (str(len(pr1887_whole)) if pr1887_whole is not None else "NA"),
        "pr1887_whole_compatible=" +
        (str(not disallowed).lower() if disallowed is not None else "NA"),
        f"upstream_completed={str(upstream is not None).lower()}",
        "upstream_pr1887_differences=" +
        (str(len(upstream_pr1887)) if upstream_pr1887 is not None else "NA"),
        f"result_sha256={digest(result_file)}",
    )
    if disallowed:
        raise RuntimeError("Original+PR1887 has an unclassified semantic difference")


if __name__ == "__main__":
    main()
