#!/usr/bin/env python3
"""Revalidate downloaded three-layout screening logs against archived gates."""
import argparse
import hashlib
import json
import re
import statistics
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition, message):
    if not condition:
        raise ValueError(message)


def seconds(text):
    result = 0
    for part in text.split(":"):
        result = result * 60 + float(part)
    return result


def validate_run(directory, row):
    log = (directory / "analysis.log").read_text()
    timing = (directory / "time.txt").read_text()
    raw = json.loads((directory / "record.json").read_text())
    require(all(row[k] == v for k, v in raw.items()), "record/run mismatch")
    patterns = {
        "ae_seconds": (log, r"^Total_Time\(sec\)\s+(.+)$"),
        "function_coverage_percent": (log, r"^Func_Coverage_Percent\s+(.+)$"),
        "icfg_node_trace": (log, r"^ICFG_Node_Trace\s+(.+)$"),
        "wall_clock": (timing, r"^\s*Elapsed \(wall clock\) time \(h:mm:ss or m:ss\):\s*(.+)$"),
        "max_rss_kib": (timing, r"^\s*Maximum resident set size \(kbytes\):\s*(\d+)$"),
    }
    for key, (text, pattern) in patterns.items():
        found = re.findall(pattern, text, re.M)
        require(len(found) == 1 and found[0] == row[key], f"raw {key} mismatch")
    require(re.findall(r"^\s*Exit status:\s*(\d+)$", timing, re.M) == ["0"],
            "nonzero/missing raw exit status")
    require(row["exit_code"] == 0 and row["completed"], "incomplete run")
    return {name: digest(directory / name) for name in ("analysis.log", "record.json", "time.txt")}


def collect(root, gates_root):
    adaptive = json.loads((gates_root / "2026-09-20-box-adaptive-semantic-final.json").read_text())
    packed = json.loads((gates_root / "2026-09-20-box-page-semantic-final.json").read_text())
    gates = {}
    for row in adaptive["rows"]:
        d = row["record"]
        require(d["passed"], "failed adaptive gate")
        gates["adaptive", d["mode"], d["program"]] = (
            row["sha256"], d["manifest"], adaptive["variant_manifests"][d["variants_ref"]], d["runs"])
    for row in packed["rows"]:
        require(row["status"] == "passed", "failed packed gate")
        gates["packed", row["mode"], row["program"]] = (
            row["source_sha256"], row, packed["runtime_manifests"][row["mode"]], row["runs"])
    result = {"measurement_class": "concurrent-shared-host-screening", "isolated": False,
              "source_root": str(root), "collector_sha256": digest(Path(__file__)),
              "gate_files": {name: digest(gates_root / name) for name in
                  ("2026-09-20-box-adaptive-semantic-final.json", "2026-09-20-box-page-semantic-final.json")},
              "manifests": {}, "contexts": {}, "rows": [], "aggregate": []}
    seen = set()
    for file in sorted(root.glob("page-adaptive-performance-screening-v1-*/**/result.json")):
        d = json.loads(file.read_text())
        candidate, = set(d["manifest"]["variants"]) - {"inline"}
        key = candidate, d["mode"], d["program"]
        require(key not in seen, "duplicate case")
        seen.add(key)
        sha, gate_manifest, variants, gate_runs = gates[key]
        require(d["passed"] and d["phase"] == "performance" and len(d["runs"]) == 8, "incomplete block")
        require(d["semantic_result_sha256"] == sha, "semantic prerequisite checksum differs")
        for field in ("input_sha256", "extapi_sha256"):
            require(d["manifest"][field] == gate_manifest[field], f"{field} changed")
        require(d["manifest"]["variants"] == variants, "binary/runtime fingerprint changed")
        context_key = d["mode"] + "/" + d["program"]
        cf = root / "page-adaptive-performance-screening-v1-context" / (context_key + ".json")
        context = json.loads(cf.read_text())
        require(context["not_isolated"] and context["host"] == d["host"], "incorrect host/isolation metadata")
        blocks = context["blocks"]
        require(len(blocks) == 2 and {b["candidate"] for b in blocks} == {"adaptive", "packed"}
                and all(b["exit_code"] == 0 for b in blocks)
                and context["started"] <= blocks[0]["started"] <= blocks[0]["finished"]
                <= blocks[1]["started"] <= blocks[1]["finished"] <= context["finished"], "invalid sequential blocks")
        result["contexts"][context_key] = {"sha256": digest(cf), "record": context}
        variant_key = hashlib.sha256(json.dumps(variants, sort_keys=True).encode()).hexdigest()
        result["manifests"][variant_key] = d["manifest"].pop("variants")
        d["manifest"]["variants_ref"] = variant_key
        log_hashes, metrics = {}, {}
        for row in d["runs"]:
            run_key = f'{row["layout"]}-{row["repeat"]}'
            require(run_key not in log_hashes, "duplicate run")
            require(row["warmup"] == (row["repeat"] == -1), "warmup/repeat mismatch")
            log_hashes[run_key] = validate_run(file.parent / run_key, row)
            for field in ("function_coverage_percent", "icfg_node_trace"):
                require(all(gr[field] == [row[field]] for gr in gate_runs), "gate/workload mismatch")
        require(set(log_hashes) == {f"{layout}-{repeat}" for layout in ("inline", candidate)
                                   for repeat in (-1, 0, 1, 2)}, "missing/extra run")
        for layout in ("inline", candidate):
            runs = [r for r in d["runs"] if r["layout"] == layout and not r["warmup"]]
            values = {"wall_seconds": [seconds(r["wall_clock"]) for r in runs],
                      "ae_seconds": [float(r["ae_seconds"]) for r in runs],
                      "rss_mib": [int(r["max_rss_kib"]) / 1024 for r in runs]}
            metrics[layout] = {k: {"median": statistics.median(v), "min": min(v), "max": max(v)}
                               for k, v in values.items()}
        ratios = {k: metrics[candidate][k]["median"] / metrics["inline"][k]["median"]
                  for k in metrics["inline"]}
        result["rows"].append({"candidate": candidate, "program": d["program"], "mode": d["mode"],
            "source_relative_path": str(file.relative_to(root)), "source_sha256": digest(file),
            "record": d, "raw_log_sha256": log_hashes, "context_ref": context_key,
            "metrics": metrics, "ratios_to_own_inline": ratios})
    require(seen == set(gates) and len(seen) == 40, "campaign case set incomplete")
    for mode in ("semi-sparse", "sparse"):
        for candidate in ("adaptive", "packed"):
            rows = [r for r in result["rows"] if r["mode"] == mode and r["candidate"] == candidate]
            result["aggregate"].append({"mode": mode, "candidate": candidate, "cases": len(rows),
                "geomean_ratios": {k: statistics.geometric_mean(r["ratios_to_own_inline"][k] for r in rows)
                                   for k in ("wall_seconds", "ae_seconds", "rss_mib")}})
    result["counts"] = {"tasks": 20, "blocks": 40, "executions": 320, "measured": 240, "warmup": 80,
                        "raw_logs_revalidated": 320}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--gates", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    require(not args.output.exists(), "refuse to overwrite prior result")
    result = collect(args.root, args.gates)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"counts": result["counts"], "aggregate": result["aggregate"]}, indent=2))


if __name__ == "__main__":
    main()
