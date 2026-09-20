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
    work = {}
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
        elif line.startswith("BOX_STORAGE_WORK "):
            fields = [item.split("=", 1) for item in line.split()[1:]]
            values = dict(fields)
            if len(values) != len(fields):
                raise ValueError("duplicate work field")
            kind = values.pop("kind")
            if kind in work:
                raise ValueError("duplicate work record")
            work[kind] = {key: int(value) for key, value in values.items()}
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
    if work:
        expected = {"clone", "grow", "shrink", "promote", "demote", "insert", "update", "erase"}
        fields = {"count", "direct", "occupied_slots", "copied_slots", "relocated_slots",
                  "allocated_slot_bytes", "peak_operation_overlap_slot_bytes"}
        if set(work) != expected:
            raise ValueError("incomplete work records")
        for kind, row in work.items():
            if set(row) != fields or any(v < 0 for v in row.values()):
                raise ValueError("invalid work fields")
            if (row["direct"] > row["count"] or
                    any(row[key] > 8 * row["count"] for key in
                        ("occupied_slots", "copied_slots", "relocated_slots")) or
                    (row["count"] == 0 and any(row.values()))):
                raise ValueError("inconsistent work counts")
            if kind in ("insert", "update", "erase") and row["allocated_slot_bytes"]:
                raise ValueError("mutation allocation must use separate work record")
        if work["clone"]["count"] != events["detach"] + events["join_materialized"]:
            raise ValueError("clone work/event mismatch")
        if (work["promote"]["direct"] != work["promote"]["count"] or
                any(work[k]["direct"] for k in ("grow", "shrink", "demote"))):
            raise ValueError("work format mismatch")
        result["work"] = work
    return result


def fingerprint(build, layout, canonical=False):
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
    result = {"identity": identity,
              "ldd": re.sub(r"\(0x[0-9a-fA-F]+\)", "(address)", linkage)}
    if canonical:
        observer = build / "bin/box-page-semantic-observer"
        paths.append(observer)
        link = subprocess.check_output(["ldd", str(observer)], text=True)
        loaded = re.search(r"libAbstractDomainCore\S* => (\S+)", link)
        if not loaded or Path(loaded[1]).resolve() != core.resolve():
            raise ValueError("canonical observer library override")
        result["canonical_ldd"] = re.sub(r"\(0x[0-9a-fA-F]+\)", "(address)", link)
    result["files"] = {str(p): digest(p) for p in paths}
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--study", type=Path, required=True)
    parser.add_argument("--program", required=True)
    parser.add_argument("--mode", choices=("semi-sparse", "sparse"), required=True)
    parser.add_argument("--candidate", choices=("packed", "adaptive"), default="packed")
    parser.add_argument("--first", choices=("inline", "packed", "adaptive"), default="inline")
    parser.add_argument("--cap-seconds", type=int, required=True)
    parser.add_argument("--build-suffix", default="census-v1")
    parser.add_argument("--result-set", default="page-layout-census-v1")
    parser.add_argument("--require-occupancy", action="store_true")
    parser.add_argument("--require-work", action="store_true")
    args = parser.parse_args()
    if args.first not in ("inline", args.candidate):
        parser.error("first must be inline or the selected candidate")
    gate_set = "page-adaptive-semantic-v1" if args.candidate == "adaptive" else "page-layout-semantic-v1"
    gate_path = args.study / "results" / gate_set / args.mode / args.program / "result.json"
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
    canonical_helper = Path(__file__).with_name("run-t5-semantic-case.py")
    semantic_run = runpy.run_path(str(canonical_helper))["run_variant"]
    state = {"passed": False, "program": args.program, "mode": args.mode,
             "host": platform.node(), "runs": [], "timing_class": "instrumented-census",
             "runner_sha256": digest(__file__), "helper_sha256": digest(helper),
             "gate_sha256": digest(gate_path), "cap_seconds": args.cap_seconds,
             "candidate": args.candidate, "canonical_runs": [],
             "canonical_helper_sha256": digest(canonical_helper),
             "scope": "retained physical pages plus whole-run slot work; no publication/lifetime census or phase timing"}

    def save():
        (output / "result.json").write_text(json.dumps(state, indent=2) + "\n")

    try:
        os.environ.pop("BOX_STORAGE_CENSUS_ONLY", None)
        os.environ.pop("AUDIT_MEMORY_POLICY", None)
        order = [args.first, args.candidate if args.first == "inline" else "inline"]
        audits = []
        for layout in order:
            build = args.study / f"build-page-{layout}-{args.build_suffix}"
            before = fingerprint(build, layout, args.require_work)
            if args.require_work:
                _, canonical = semantic_run(build / "bin/box-page-semantic-observer", extapi,
                                             bitcode, args.cap_seconds, output / (layout + "-canonical"), args.mode)
                canonical["layout"] = layout
                state["canonical_runs"].append(canonical)
                save()
                if not canonical["completed"] or any(
                        canonical[key] != old[key] for old in gate["runs"] for key in
                        ("projection_sha256", "counts", "function_coverage_percent", "icfg_node_trace")):
                    raise ValueError("work diagnostic canonical gate mismatch")
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
            if args.require_work and ("work" not in record or "occupancy" not in record):
                raise ValueError("missing required physical work records")
            if not audit:
                raise ValueError("empty value audit")
            audits.append(audit)
            for key in ("function_coverage_percent", "icfg_node_trace"):
                if [record[key]] != gate["runs"][0][key]:
                    raise ValueError(f"telemetry workload differs: {key}")
            if fingerprint(build, layout, args.require_work) != before:
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
        if args.require_work:
            for kind in ("insert", "update", "erase"):
                if state["runs"][0]["work"][kind]["count"] != state["runs"][1]["work"][kind]["count"]:
                    raise ValueError("unexpected mutation work count difference")
        state["passed"] = True
    except Exception as error:
        state["error"] = str(error)
        raise
    finally:
        save()
    print(json.dumps(state))


if __name__ == "__main__":
    main()
