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
CAPACITIES = (64, 256, 1024, 4096)


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
    states = None
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
        elif line.startswith("BOX_OPERATION_STATES "):
            if states is not None:
                raise ValueError("duplicate operation state row")
            states = {key: int(value) for key, value in fields(line).items()}
        elif line.startswith("BOX_OPERATION_CACHE "):
            values = fields(line)
            capacity = int(values.pop("capacity"))
            if capacity in caches:
                raise ValueError("duplicate operation cache row")
            caches[capacity] = {key: int(value)
                                for key, value in values.items()}

    if tuple(summaries) != OPERATIONS or states is None:
        raise ValueError("incomplete operation summaries")
    if set(caches) != set(CAPACITIES) or set(lru) != {
            (operation, capacity) for operation in OPERATIONS
            for capacity in CAPACITIES}:
        raise ValueError("incomplete operation cache census")
    summary_fields = {"calls", "tracked", "dropped", "elapsed_ns",
                      "normalization_ns", "result_canonical_bytes"}
    for operation, row in summaries.items():
        if set(row) != summary_fields or any(value < 0 for value in row.values()):
            raise ValueError(f"invalid {operation} summary")
        if row["tracked"] + row["dropped"] != row["calls"]:
            raise ValueError(f"inconsistent {operation} coverage")
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
    if set(states) != {"unique", "canonical_bytes", "canonical_budget_bytes",
                       "entry_shallow_bytes", "pending"}:
        raise ValueError("invalid operation state fields")
    if (any(value < 0 for value in states.values()) or states["pending"] != 0 or
            states["canonical_bytes"] > states["canonical_budget_bytes"]):
        raise ValueError("invalid operation state budget")
    for capacity, row in caches.items():
        if set(row) != {"entries", "peak_result_canonical_bytes",
                        "key_shallow_bytes"}:
            raise ValueError("invalid operation cache fields")
        if any(value < 0 for value in row.values()) or row["entries"] > capacity:
            raise ValueError("invalid operation cache occupancy")

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
            "states": states, "caches": caches,
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--gate", type=Path, required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--mode", choices=("semi-sparse", "sparse"), required=True)
    parser.add_argument("--cap-seconds", type=int, required=True)
    parser.add_argument("--state-budget-bytes", type=int,
                        default=256 * 1024 * 1024)
    parser.add_argument("--result-set", default="operation-reuse-census-v1")
    args = parser.parse_args()
    if args.state_budget_bytes < 0:
        parser.error("state budget must be non-negative")

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
             "state_budget_bytes": args.state_budget_bytes,
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

        previous_budget = os.environ.get("BOX_OPERATION_STATE_BUDGET_BYTES")
        os.environ["BOX_OPERATION_STATE_BUDGET_BYTES"] = \
            str(args.state_budget_bytes)
        try:
            observer_dir = output / "observer"
            _, observer = run_variant(args.build / "bin/box-storage-observer",
                                      extapi, bitcode, args.cap_seconds,
                                      observer_dir, args.mode)
        finally:
            if previous_budget is None:
                os.environ.pop("BOX_OPERATION_STATE_BUDGET_BYTES", None)
            else:
                os.environ["BOX_OPERATION_STATE_BUDGET_BYTES"] = previous_budget
        observer["role"] = "operation-census"
        observer["operations"] = parse_operations(
            (observer_dir / "analysis.log").read_text(errors="replace"))
        state["runs"].append(observer)
        save()
        if not observer["completed"]:
            raise ValueError("operation census observer failed")
        semantic_keys = ("projection_sha256", "counts",
                         "function_coverage_percent", "icfg_node_trace")
        if any(observer[key] != canonical[key] for key in semantic_keys):
            raise ValueError("operation census changed canonical projection")
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
