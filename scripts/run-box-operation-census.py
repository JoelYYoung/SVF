#!/usr/bin/env python3
"""Bounded pure-operation reuse census; instrumented time is not performance evidence."""

import argparse
import hashlib
import json
import os
import re
import runpy
import subprocess
from pathlib import Path


OPERATIONS = ("join", "meet", "widen", "narrow", "subset", "equivalent")
CAPACITIES = (64, 256)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def fields(line):
    pairs = [item.split("=", 1) for item in line.split()[1:]]
    values = dict(pairs)
    if len(values) != len(pairs):
        raise ValueError("duplicate operation census field")
    return values


def parse_operations(text):
    summaries = {}
    lru = {}
    caches = {}
    window = None
    for line in text.splitlines():
        if line.startswith("BOX_OPERATION_SUMMARY "):
            values = fields(line)
            operation = values.pop("op")
            if operation in summaries:
                raise ValueError("duplicate operation summary")
            summaries[operation] = {key: int(value)
                                    for key, value in values.items()}
        elif line.startswith("BOX_OPERATION_LRU "):
            values = fields(line)
            key = (values.pop("op"), int(values.pop("capacity")))
            if key in lru:
                raise ValueError("duplicate operation LRU row")
            lru[key] = {name: int(value) for name, value in values.items()}
        elif line.startswith("BOX_OPERATION_WINDOW "):
            if window is not None:
                raise ValueError("duplicate operation window row")
            window = {key: int(value) for key, value in fields(line).items()}
        elif line.startswith("BOX_OPERATION_CACHE "):
            values = fields(line)
            capacity = int(values.pop("capacity"))
            if capacity in caches:
                raise ValueError("duplicate operation cache row")
            caches[capacity] = {key: int(value)
                                for key, value in values.items()}

    if tuple(summaries) != OPERATIONS or window is None:
        raise ValueError("incomplete operation summaries")
    expected_lru = {
            (operation, capacity) for operation in OPERATIONS
            for capacity in CAPACITIES}
    if set(caches) != set(CAPACITIES) or set(lru) != expected_lru:
        raise ValueError("incomplete operation cache census")
    summary_fields = {"calls", "tracked", "dropped", "elapsed_ns",
                      "normalization_ns", "result_canonical_bytes"}
    for operation, row in summaries.items():
        if set(row) != summary_fields or any(value < 0 for value in row.values()):
            raise ValueError(f"invalid {operation} summary")
        if row["tracked"] + row["dropped"] != row["calls"]:
            raise ValueError(f"inconsistent {operation} coverage")
        if row["dropped"] != 0:
            raise ValueError(f"incomplete {operation} exact window")
        prior_hits = -1
        prior_hit_time = -1
        for capacity in CAPACITIES:
            reuse = lru[(operation, capacity)]
            if set(reuse) != {"hits", "hit_elapsed_ns"}:
                raise ValueError("invalid LRU fields")
            if (reuse["hits"] < prior_hits or
                    reuse["hit_elapsed_ns"] < prior_hit_time or
                    reuse["hits"] > row["tracked"] or
                    reuse["hit_elapsed_ns"] > row["elapsed_ns"]):
                raise ValueError(f"invalid {operation} LRU monotonicity")
            prior_hits = reuse["hits"]
            prior_hit_time = reuse["hit_elapsed_ns"]
    if (set(window) != {"exact", "pending"} or
            window != {"exact": 1, "pending": 0}):
        raise ValueError("invalid exact operation window")
    for capacity, row in caches.items():
        if set(row) != {"entries", "peak_operand_canonical_bytes",
                       "peak_result_canonical_bytes", "key_shallow_bytes",
                       "exact_mismatches"}:
            raise ValueError("invalid operation exact cache fields")
        if (any(value < 0 for value in row.values()) or
                row["entries"] > capacity):
            raise ValueError("invalid operation exact cache occupancy")

    total_elapsed = sum(row["elapsed_ns"] for row in summaries.values())
    ideal = {}
    for capacity in CAPACITIES:
        hit_elapsed = sum(lru[(operation, capacity)]["hit_elapsed_ns"]
                          for operation in OPERATIONS)
        ideal[str(capacity)] = {
            "hits": sum(lru[(operation, capacity)]["hits"]
                        for operation in OPERATIONS),
            "hit_elapsed_ns": hit_elapsed,
            "measured_operation_fraction":
                hit_elapsed / total_elapsed if total_elapsed else 0.0,
        }
    return {"summaries": summaries,
            "lru": {f"{operation}/{capacity}": row
                    for (operation, capacity), row in lru.items()},
            "window": window, "caches": caches,
            "total_elapsed_ns": total_elapsed,
            "ideal_removable_operation_cost": ideal}


def fingerprint(build):
    paths = [build / "bin/box-storage-observer",
             build / "bin/box-page-semantic-observer",
             build / "lib/libAbstractDomainCore.so.3.4",
             build / "lib/libSvfCore.so.3.4",
             build / "lib/libSvfLLVM.so.3.4",
             build / "CMakeCache.txt"]
    for path in paths:
        if not path.exists():
            raise ValueError(f"missing census artifact: {path}")
    linkage = subprocess.check_output(
        ["ldd", str(paths[0])], text=True)
    loaded = re.search(r"libAbstractDomainCore\S* => (\S+)", linkage)
    if not loaded or Path(loaded[1]).resolve() != paths[2].resolve():
        raise ValueError("operation observer library override")
    return {"files": {str(path): digest(path) for path in paths},
            "ldd": re.sub(r"\(0x[0-9a-fA-F]+\)", "(address)", linkage)}


def observer_run(executable, extapi, bitcode, cap, directory, mode,
                 operation_census):
    directory.mkdir()
    log = directory / "analysis.log"
    timing = directory / "time.txt"
    command = [str(executable), f"-ae-sparsity={mode}",
               "-ae-fun-entry=main", "-stat=true", f"-extapi={extapi}",
               str(bitcode)]
    wrapped = ["/usr/bin/time", "-v", "-o", str(timing),
               "timeout", "--signal=TERM", str(cap), *command]
    environment = os.environ.copy()
    environment.pop("BOX_OPERATION_CENSUS", None)
    if operation_census:
        environment["BOX_OPERATION_CENSUS"] = "1"
    with log.open("w") as stream:
        completed = subprocess.run(wrapped, stdout=stream,
                                   stderr=subprocess.STDOUT, env=environment)
    log_text = log.read_text(errors="replace")
    time_text = timing.read_text(errors="replace") if timing.exists() else ""
    audits = sorted(line for line in log_text.splitlines()
                    if line.startswith("AUDIT "))
    coverage = re.findall(r"^Func_Coverage_Percent\s+(\S+)",
                          log_text, re.MULTILINE)
    trace = re.findall(r"^ICFG_Node_Trace\s+(\S+)",
                       log_text, re.MULTILINE)
    wall = re.findall(r"Elapsed \(wall clock\) time.*:\s*(\S+)", time_text)
    rss = re.findall(r"Maximum resident set size \(kbytes\):\s*(\d+)",
                     time_text)
    return {
        "command": command,
        "exit_code": completed.returncode,
        "completed": completed.returncode == 0,
        "executable_sha256": digest(executable),
        "log_sha256": digest(log),
        "audit_entries": len(audits),
        "audit_sha256": hashlib.sha256("\n".join(audits).encode()).hexdigest(),
        "function_coverage_percent": coverage,
        "icfg_node_trace": trace,
        "wall_clock": wall,
        "max_rss_kib": rss,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--gate", type=Path, required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--mode", choices=("semi-sparse", "sparse"), required=True)
    parser.add_argument("--cap-seconds", type=int, required=True)
    parser.add_argument("--result-set", default="operation-reuse-census-v1")
    args = parser.parse_args()

    gate = json.loads(args.gate.read_text())
    if not gate["passed"] or gate["program"] != args.program or \
            gate["mode"] != args.mode:
        raise ValueError("operation census requires matching semantic gate")
    manifest = gate.get("manifest", gate)
    bitcode = Path(manifest["input"])
    extapi = Path(manifest["extapi"])
    if digest(bitcode) != manifest["input_sha256"] or \
            digest(extapi) != manifest["extapi_sha256"]:
        raise ValueError("semantic gate inputs changed")

    output = (args.study / "results" / args.result_set /
              args.mode / args.program)
    output.mkdir(parents=True, exist_ok=False)
    helper = Path(__file__).with_name("run-t5-semantic-case.py")
    run_variant = runpy.run_path(str(helper))["run_variant"]
    state = {"passed": False, "program": args.program, "mode": args.mode,
             "phase": "instrumented-operation-census",
             "timing_class": "instrumented-not-performance-evidence",
             "source_commit": subprocess.check_output(
                 ["git", "rev-parse", "HEAD"], cwd=Path(__file__).parent.parent,
                 text=True).strip(),
             "gate": str(args.gate), "gate_sha256": digest(args.gate),
             "helper_sha256": digest(helper), "runner_sha256": digest(__file__),
             "cap_seconds": args.cap_seconds, "runs": []}

    def save():
        (output / "result.json").write_text(json.dumps(state, indent=2) + "\n")

    try:
        before = fingerprint(args.build)
        canonical_executable = args.build / "bin/box-page-semantic-observer"
        canonical_dir = output / "canonical"
        _, canonical = run_variant(canonical_executable, extapi, bitcode,
                                   args.cap_seconds, canonical_dir, args.mode)
        canonical["role"] = "semantic-control"
        state["runs"].append(canonical)
        save()
        if not canonical["completed"]:
            raise ValueError("operation census semantic control failed")

        observer_executable = args.build / "bin/box-storage-observer"
        control_dir = output / "audit-control"
        control = observer_run(observer_executable, extapi, bitcode,
                               args.cap_seconds, control_dir, args.mode, False)
        control["role"] = "audit-control"
        state["runs"].append(control)
        save()
        if not control["completed"] or not control["audit_entries"]:
            raise ValueError("operation census audit control failed")

        observer_dir = output / "observer"
        observer = observer_run(observer_executable, extapi, bitcode,
                                args.cap_seconds, observer_dir, args.mode, True)
        observer["role"] = "operation-census"
        observer["operations"] = parse_operations(
            (observer_dir / "analysis.log").read_text(errors="replace"))
        state["runs"].append(observer)
        save()
        if not observer["completed"]:
            raise ValueError("operation census observer failed")
        audit_keys = ("audit_entries", "audit_sha256",
                      "function_coverage_percent", "icfg_node_trace")
        if any(observer[key] != control[key] for key in audit_keys):
            raise ValueError("operation census changed observer audit")
        semantic_keys = ("projection_sha256", "counts",
                         "function_coverage_percent", "icfg_node_trace")
        for key in semantic_keys:
            if any(run[key] != canonical[key] for run in gate["runs"]):
                raise ValueError(f"operation census gate mismatch: {key}")
        if fingerprint(args.build) != before:
            raise ValueError("operation census binary changed during run")
        state.update(passed=True, fingerprint=before)
    except Exception as error:
        state["error"] = str(error)
        save()
        raise
    save()


if __name__ == "__main__":
    main()
