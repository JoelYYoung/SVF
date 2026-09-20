#!/usr/bin/env python3
"""Physical-page census only; instrumented timings are never performance evidence."""
import argparse
import hashlib
import json
import os
import platform
import re
import runpy
import subprocess
from pathlib import Path


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def parse_storage(text):
    carriers = {}
    events = None
    occupancy = {}
    for line in text.splitlines():
        if line.startswith("BOX_STORAGE_CARRIER "):
            values = dict(item.split("=", 1) for item in line.split()[1:])
            role = values.pop("role")
            if role in carriers:
                raise ValueError("duplicate carrier")
            carriers[role] = {key: int(value) for key, value in values.items()}
        elif line.startswith("BOX_STORAGE_EVENTS "):
            if events is not None:
                raise ValueError("duplicate event record")
            events = {key: int(value) for key, value in
                      (item.split("=", 1) for item in line.split()[1:])}
        elif line.startswith("BOX_STORAGE_OCCUPANCY "):
            fields = [item.split("=", 1) for item in line.split()[1:]]
            values = dict(fields)
            if len(values) != len(fields):
                raise ValueError("duplicate occupancy field")
            scope, name = values.pop("scope"), values.pop("name")
            key = f"{scope}:{name}"
            if key in occupancy:
                raise ValueError("duplicate occupancy record")
            bins = {}
            for field, value in values.items():
                if not re.fullmatch(r"used(0|[1-9][0-9]*)", field):
                    raise ValueError("invalid occupancy bucket")
                count = int(value)
                if count <= 0:
                    raise ValueError("invalid occupancy count")
                bins[int(field[4:])] = count
            occupancy[key] = bins
    if len(carriers) != 2 or events is None:
        raise ValueError("missing storage census")
    result = {"carriers": carriers, "events": events}
    # Old frozen census logs remain readable. New logs must be complete and
    # self-consistent; partial histograms must never look like zero observations.
    if occupancy:
        event_names = {"allocate_empty": "allocate", "detach_before": "detach",
                       "release": "release", "write_unique_before": "write_unique",
                       "erase_unique_before": "erase_unique", "join_shared": "join_shared",
                       "join_clone_before": "join_materialized"}
        expected = {f"event:{name}" for name in event_names}
        expected.update(f"retained:{role}" for role in carriers)
        if set(occupancy) != expected:
            raise ValueError("incomplete occupancy records")
        for name, event in event_names.items():
            if sum(occupancy[f"event:{name}"].values()) != events[event]:
                raise ValueError("occupancy event total mismatch")
        for role, carrier in carriers.items():
            bins = occupancy[f"retained:{role}"]
            if (sum(bins.values()) != carrier["unique_pages"] or
                    sum(used * count for used, count in bins.items()) != carrier["occupied_slots"]):
                raise ValueError("occupancy retained total mismatch")
        result["occupancy"] = occupancy
    return result


def fingerprint(build, layout):
    identity = subprocess.check_output(
        [str(build / "bin/box-page-contract"), "--identity"], text=True).strip()
    if identity != f"representation=chunk8/{layout}8":
        raise ValueError(f"wrong census layout: {identity}")
    executable = build / "bin/box-storage-observer"
    core = build / "lib/libAbstractDomainCore.so.3.4"
    linkage = subprocess.check_output(["ldd", str(executable)], text=True)
    resolved = re.search(r"libAbstractDomainCore\S* => (\S+)", linkage)
    if not resolved or Path(resolved[1]).resolve() != core.resolve():
        raise ValueError("census library override")
    paths = [executable, build / "bin/box-page-contract", core,
             build / "lib/libSvfCore.so.3.4", build / "lib/libSvfLLVM.so.3.4",
             build / "CMakeCache.txt"]
    return {"identity": identity, "files": {str(p): digest(p) for p in paths},
            "ldd": re.sub(r"\(0x[0-9a-fA-F]+\)", "(address)", linkage)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--mode", choices=("semi-sparse", "sparse"), required=True)
    parser.add_argument("--first", choices=("inline", "packed"), default="inline")
    parser.add_argument("--cap-seconds", type=int, required=True)
    parser.add_argument("--build-suffix", default="census-v1")
    parser.add_argument("--result-set", default="page-layout-census-v1")
    parser.add_argument("--require-occupancy", action="store_true")
    args = parser.parse_args()
    gate_path = args.study / "results/page-layout-semantic-v1" / args.mode / args.program / "result.json"
    gate = json.loads(gate_path.read_text())
    if not gate["passed"] or gate["mode"] != args.mode or gate["program"] != args.program:
        raise ValueError("census requires successful matching semantic gate")
    if gate["host"] != platform.node():
        raise ValueError("census host differs from gate")
    bitcode, extapi = (Path(gate["manifest"][key]) for key in ("input", "extapi"))
    for key, path in (("input", bitcode), ("extapi", extapi)):
        if digest(path) != gate["manifest"][key + "_sha256"]:
            raise ValueError(f"{key} changed")
    output = args.study / "results" / args.result_set / args.mode / args.program
    output.mkdir(parents=True, exist_ok=False)
    helper = args.study / "page-runner-v1/scripts/run-t5-storage-performance-case.py"
    run_variant = runpy.run_path(str(helper))["run_variant"]
    state = {"passed": False, "program": args.program, "mode": args.mode,
             "host": platform.node(), "runs": [], "timing_class": "instrumented-census",
             "runner_sha256": digest(__file__), "helper_sha256": digest(helper),
             "gate_sha256": digest(gate_path), "cap_seconds": args.cap_seconds,
             "scope": "retained physical pages; peak page count, not peak byte census"}

    def save():
        (output / "result.json").write_text(json.dumps(state, indent=2) + "\n")

    try:
        os.environ.pop("BOX_STORAGE_CENSUS_ONLY", None)
        os.environ.pop("AUDIT_MEMORY_POLICY", None)
        order = [args.first, "packed" if args.first == "inline" else "inline"]
        audits = []
        for layout in order:
            build = args.study / f"build-page-{layout}-{args.build_suffix}"
            before = fingerprint(build, layout)
            record = run_variant(build / "bin/box-storage-observer", extapi,
                                 bitcode, args.cap_seconds, output / layout, args.mode)
            record.update(layout=layout, fingerprint=before)
            log = output / layout / "analysis.log"
            text = log.read_text()
            record.update(parse_storage(text))
            record["log_sha256"] = digest(log)
            audit = sorted(line for line in text.splitlines() if line.startswith("AUDIT "))
            record["audit_entries"] = len(audit)
            record["audit_sha256"] = hashlib.sha256("\n".join(audit).encode()).hexdigest()
            state["runs"].append(record)
            save()
            if args.require_occupancy and "occupancy" not in record:
                raise ValueError("missing required occupancy histograms")
            if not audit:
                raise ValueError("empty value audit")
            audits.append(audit)
            for key in ("function_coverage_percent", "icfg_node_trace"):
                if [record[key]] != gate["runs"][0][key]:
                    raise ValueError(f"telemetry workload differs: {key}")
            if fingerprint(build, layout) != before:
                raise ValueError("census binary changed during run")
        if audits[0] != audits[1]:
            raise ValueError("census load/binary value projections differ")
        # Payload bytes may differ. All observed graph, occupancy and COW counts must match.
        for role in state["runs"][0]["carriers"]:
            left, right = [dict(run["carriers"][role]) for run in state["runs"]]
            left.pop("unique_page_shallow_bytes")
            right.pop("unique_page_shallow_bytes")
            if left != right:
                raise ValueError(f"unexpected structural census difference: {role}")
        if state["runs"][0]["events"] != state["runs"][1]["events"]:
            raise ValueError("unexpected COW event difference")
        if state["runs"][0].get("occupancy") != state["runs"][1].get("occupancy"):
            raise ValueError("unexpected occupancy distribution difference")
        state["passed"] = True
    except Exception as error:
        state["error"] = str(error)
        raise
    finally:
        save()
    print(json.dumps(state))


if __name__ == "__main__":
    main()
