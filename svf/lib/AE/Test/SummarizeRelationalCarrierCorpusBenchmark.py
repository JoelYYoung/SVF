#!/usr/bin/env python3
"""Audit and summarize production relational-carrier corpus runs.

The input CSV files remain the source of truth.  This script never drops a
failed or timed-out attempt: it produces a merged ledger, an Octagon semantic
equivalence audit, per-candidate completion/resource statistics, and paired
ratios over explicitly reported common completers.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import math
import statistics
from collections import Counter, defaultdict
from pathlib import Path


DEFAULT_CANDIDATES = (
    "box",
    "octagon-dense-half",
    "octagon-sparse-finite",
    "octagon-component-dense",
    "polyhedra-native-hv",
)
OCTAGON_CANDIDATES = (
    "octagon-dense-half",
    "octagon-sparse-finite",
    "octagon-component-dense",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path)
    parser.add_argument("--merged-output", required=True, type=Path)
    parser.add_argument("--audit-output", required=True, type=Path)
    parser.add_argument("--candidate-summary-output", required=True, type=Path)
    parser.add_argument("--pairwise-summary-output", required=True, type=Path)
    parser.add_argument("--shape-summary-output", type=Path)
    parser.add_argument("--telemetry-output", type=Path)
    parser.add_argument("--telemetry-audit-output", type=Path)
    parser.add_argument(
        "--candidate", action="append", choices=DEFAULT_CANDIDATES,
        help="Expected candidate; repeat to override the five-candidate default.",
    )
    return parser.parse_args()


def percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    rank = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[rank]


def number(row: dict[str, str], field: str) -> float | None:
    value = row.get(field, "")
    try:
        return float(value) if value != "" else None
    except ValueError:
        return None


def write_csv(path: Path, fields: list[str], rows: list[dict[str, object]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def load_rows(paths: list[Path]) -> tuple[list[str], list[dict[str, str]]]:
    fields: list[str] | None = None
    by_run_id: dict[str, dict[str, str]] = {}
    for path in paths:
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames is None:
                raise SystemExit(f"missing CSV header: {path}")
            if fields is None:
                fields = reader.fieldnames
            elif reader.fieldnames != fields:
                raise SystemExit(f"incompatible CSV header: {path}")
            for row in reader:
                run_id = row.get("run_id", "")
                if not run_id:
                    raise SystemExit(f"row without run_id: {path}:{reader.line_num}")
                previous = by_run_id.get(run_id)
                if previous is not None and previous != row:
                    raise SystemExit(f"conflicting duplicate run_id: {run_id}")
                by_run_id[run_id] = row
    if fields is None:
        raise SystemExit("no input rows")
    rows = sorted(
        by_run_id.values(),
        key=lambda row: (
            row.get("input", ""), int(row.get("repetition", "0") or 0),
            int(row.get("order_position", "0") or 0), row.get("candidate", ""),
        ),
    )
    return fields, rows


def make_audit(
    rows: list[dict[str, str]], expected: tuple[str, ...]
) -> list[dict[str, object]]:
    groups: dict[tuple[str, str], dict[str, dict[str, str]]] = defaultdict(dict)
    for row in rows:
        key = (row["input"], row["repetition"])
        candidate = row["candidate"]
        if candidate in groups[key]:
            raise SystemExit(f"duplicate candidate in input/repetition: {key} {candidate}")
        groups[key][candidate] = row

    output: list[dict[str, object]] = []
    for (input_name, repetition), candidates in sorted(groups.items()):
        missing = [name for name in expected if name not in candidates]
        oct_rows = [candidates.get(name) for name in OCTAGON_CANDIDATES]
        if any(row is None for row in oct_rows):
            octagon_gate = "incomplete"
            octagon_detail = "missing Octagon candidate"
        elif any(row["status"] != "pass" for row in oct_rows if row is not None):
            octagon_gate = "unavailable"
            octagon_detail = ";".join(
                f"{name}={candidates[name]['status']}" for name in OCTAGON_CANDIDATES
            )
        elif any(
            row.get("semantic_checksum_enabled") != "1"
            or not row.get("semantic_checksum")
            for row in oct_rows if row is not None
        ):
            octagon_gate = "not-checked"
            octagon_detail = "semantic checksum disabled or absent"
        else:
            signatures = {
                (row["analyzed_nodes"], row["semantic_states"], row["semantic_checksum"])
                for row in oct_rows if row is not None
            }
            octagon_gate = "pass" if len(signatures) == 1 else "mismatch"
            octagon_detail = "" if len(signatures) == 1 else ";".join(
                f"{name}={candidates[name]['analyzed_nodes']}/"
                f"{candidates[name]['semantic_states']}/"
                f"{candidates[name]['semantic_checksum']}"
                for name in OCTAGON_CANDIDATES
            )
        output.append({
            "input": input_name,
            "repetition": repetition,
            "all_candidates_present": int(not missing),
            "all_candidates_pass": int(
                not missing and all(candidates[name]["status"] == "pass" for name in expected)
            ),
            "missing_candidates": ";".join(missing),
            "status_by_candidate": ";".join(
                f"{name}={candidates[name]['status']}" for name in expected if name in candidates
            ),
            "octagon_semantic_gate": octagon_gate,
            "octagon_semantic_detail": octagon_detail,
        })
    return output


def make_candidate_summary(
    rows: list[dict[str, str]], expected: tuple[str, ...]
) -> list[dict[str, object]]:
    output: list[dict[str, object]] = []
    for candidate in expected:
        selected = [row for row in rows if row["candidate"] == candidate]
        status = Counter(row["status"] for row in selected)
        passed = [row for row in selected if row["status"] == "pass"]
        times = [value for row in passed if (value := number(row, "seconds")) is not None]
        rss = [value for row in passed if (value := number(row, "peak_rss_bytes")) is not None]
        output.append({
            "candidate": candidate,
            "attempts": len(selected),
            "distinct_inputs": len({row["input"] for row in selected}),
            "pass": status["pass"],
            "timeout": status["timeout"],
            "memory_limit": status["memory-limit"],
            "dimension_limit": status["dimension-limit"],
            "other_failure": len(selected) - status["pass"] - status["timeout"]
                - status["memory-limit"] - status["dimension-limit"],
            "completion_rate": status["pass"] / len(selected) if selected else "",
            "median_seconds_pass": statistics.median(times) if times else "",
            "p95_seconds_pass": percentile(times, 0.95) or "",
            "median_peak_rss_bytes_pass": statistics.median(rss) if rss else "",
            "p95_peak_rss_bytes_pass": percentile(rss, 0.95) or "",
        })
    return output


def make_pairwise_summary(
    rows: list[dict[str, str]], expected: tuple[str, ...]
) -> list[dict[str, object]]:
    indexed = {(row["input"], row["repetition"], row["candidate"]): row for row in rows}
    keys = sorted({(row["input"], row["repetition"]) for row in rows})
    output: list[dict[str, object]] = []
    for baseline, candidate in itertools.permutations(expected, 2):
        time_ratios: list[float] = []
        rss_ratios: list[float] = []
        common_inputs: set[str] = set()
        common_runs = 0
        for input_name, repetition in keys:
            base = indexed.get((input_name, repetition, baseline))
            other = indexed.get((input_name, repetition, candidate))
            if not base or not other or base["status"] != "pass" or other["status"] != "pass":
                continue
            base_time, other_time = number(base, "seconds"), number(other, "seconds")
            base_rss, other_rss = number(base, "peak_rss_bytes"), number(other, "peak_rss_bytes")
            if base_time is not None and other_time is not None and base_time > 0:
                time_ratios.append(other_time / base_time)
            if base_rss is not None and other_rss is not None and base_rss > 0:
                rss_ratios.append(other_rss / base_rss)
            common_inputs.add(input_name)
            common_runs += 1
        output.append({
            "baseline": baseline,
            "candidate": candidate,
            "common_pass_runs": common_runs,
            "distinct_common_inputs": len(common_inputs),
            "median_candidate_over_baseline_time": statistics.median(time_ratios)
                if time_ratios else "",
            "p95_candidate_over_baseline_time": percentile(time_ratios, 0.95) or "",
            "median_candidate_over_baseline_rss": statistics.median(rss_ratios)
                if rss_ratios else "",
            "p95_candidate_over_baseline_rss": percentile(rss_ratios, 0.95) or "",
        })
    return output


def dimension_bucket(dimensions: int | None) -> str:
    if dimensions is None:
        return "unknown"
    upper = 31
    while upper < dimensions and upper < 4095:
        upper = 2 * upper + 1
    return f"0-{upper}" if upper >= dimensions else "4096+"


def make_shape_summary(
    rows: list[dict[str, str]], expected: tuple[str, ...]
) -> list[dict[str, object]]:
    dimensions_by_input: dict[str, int] = {}
    for row in rows:
        value = number(row, "selected_dimensions")
        if value is not None:
            dimensions_by_input[row["input"]] = int(value)
    groups: dict[tuple[str, str], list[dict[str, str]]] = defaultdict(list)
    for row in rows:
        groups[(row["candidate"], dimension_bucket(
            dimensions_by_input.get(row["input"])))].append(row)

    output: list[dict[str, object]] = []
    for candidate in expected:
        for (group_candidate, bucket), selected in sorted(groups.items()):
            if group_candidate != candidate:
                continue
            passed = [row for row in selected if row["status"] == "pass"]
            times = [value for row in passed if (value := number(row, "seconds")) is not None]
            rss = [value for row in passed if (value := number(row, "peak_rss_bytes")) is not None]
            output.append({
                "candidate": candidate,
                "dimension_bucket": bucket,
                "attempts": len(selected),
                "distinct_inputs": len({row["input"] for row in selected}),
                "pass": len(passed),
                "completion_rate": len(passed) / len(selected),
                "median_seconds_pass": statistics.median(times) if times else "",
                "p95_seconds_pass": percentile(times, 0.95) or "",
                "median_peak_rss_bytes_pass": statistics.median(rss) if rss else "",
                "p95_peak_rss_bytes_pass": percentile(rss, 0.95) or "",
            })
    return output


def merge_telemetry(
    rows: list[dict[str, str]],
) -> tuple[list[str], list[dict[str, str]], list[dict[str, object]]]:
    telemetry_fields: list[str] | None = None
    output: list[dict[str, str]] = []
    audit: list[dict[str, object]] = []
    metadata = [
        "run_id", "host", "input", "scale", "program_family", "candidate",
        "domain", "carrier", "repetition", "runner_sha256", "bitcode_sha256",
    ]
    for run in rows:
        raw_path = run.get("telemetry_path", "")
        if not raw_path:
            audit.append({
                "run_id": run.get("run_id", ""),
                "candidate": run.get("candidate", ""),
                "run_status": run.get("status", ""),
                "telemetry_path": "",
                "telemetry_status": "not-requested",
                "observations": 0,
            })
            continue
        path = Path(raw_path)
        if not path.is_file():
            audit.append({
                "run_id": run.get("run_id", ""),
                "candidate": run.get("candidate", ""),
                "run_status": run.get("status", ""),
                "telemetry_path": raw_path,
                "telemetry_status": "missing",
                "observations": 0,
            })
            continue
        observations = 0
        with path.open(newline="", encoding="utf-8") as stream:
            reader = csv.DictReader(stream)
            if reader.fieldnames is None:
                raise SystemExit(f"telemetry file has no header: {path}")
            if telemetry_fields is None:
                telemetry_fields = reader.fieldnames
            elif telemetry_fields != reader.fieldnames:
                raise SystemExit(f"incompatible telemetry header: {path}")
            for observation in reader:
                observations += 1
                output.append({
                    **{field: run.get(field, "") for field in metadata},
                    **observation,
                })
        audit.append({
            "run_id": run.get("run_id", ""),
            "candidate": run.get("candidate", ""),
            "run_status": run.get("status", ""),
            "telemetry_path": raw_path,
            "telemetry_status": "present",
            "observations": observations,
        })
    return metadata + (telemetry_fields or []), output, audit


def main() -> None:
    options = parse_args()
    expected = tuple(options.candidate or DEFAULT_CANDIDATES)
    fields, rows = load_rows(options.inputs)
    write_csv(options.merged_output, fields, rows)
    audit = make_audit(rows, expected)
    write_csv(options.audit_output, [
        "input", "repetition", "all_candidates_present", "all_candidates_pass",
        "missing_candidates", "status_by_candidate", "octagon_semantic_gate",
        "octagon_semantic_detail",
    ], audit)
    candidate_summary = make_candidate_summary(rows, expected)
    write_csv(options.candidate_summary_output, list(candidate_summary[0]), candidate_summary)
    pairwise = make_pairwise_summary(rows, expected)
    write_csv(options.pairwise_summary_output, list(pairwise[0]), pairwise)
    if options.shape_summary_output:
        shape_summary = make_shape_summary(rows, expected)
        write_csv(options.shape_summary_output, list(shape_summary[0]), shape_summary)
    if options.telemetry_output:
        telemetry_fields, telemetry, telemetry_audit = merge_telemetry(rows)
        write_csv(options.telemetry_output, telemetry_fields, telemetry)
        if options.telemetry_audit_output:
            write_csv(options.telemetry_audit_output, [
                "run_id", "candidate", "run_status", "telemetry_path",
                "telemetry_status", "observations",
            ], telemetry_audit)
    elif options.telemetry_audit_output:
        raise SystemExit(
            "--telemetry-audit-output requires --telemetry-output")


if __name__ == "__main__":
    main()
