#!/usr/bin/env python3
"""Merge AE benchmark shards and derive timing, precision, and failure tables."""

from __future__ import annotations

import argparse
import csv
import hashlib
import statistics
from collections import defaultdict
from pathlib import Path


KEY = ("program", "sparsity", "domain")
OUTCOMES = ("Safe", "May", "Unreachable", "Unsupported")
COVERAGE = ("icfg_nodes", "analyzed_icfg_nodes", "functions", "analyzed_functions")


def read_tsv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream, delimiter="\t"))


def write_tsv(path: Path, fieldnames: list[str], rows: list[dict[str, object]]) -> None:
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def number(row: dict[str, str], field: str) -> float | None:
    value = row[field]
    return None if value in ("", "NA") else float(value)


def ledger_path(shard: Path, row: dict[str, str]) -> Path:
    return shard / (
        f"{row['program']}-{row['phase']}-r{row['run']}-"
        f"{row['sparsity']}-{row['domain']}.queries.tsv"
    )


def ledger(path: Path) -> dict[str, dict[str, str]]:
    rows = read_tsv(path)
    result: dict[str, dict[str, str]] = {}
    for row in rows:
        query_id = row["query_id"]
        if query_id in result:
            raise ValueError(f"duplicate query_id {query_id} in {path}")
        if row["outcome"] not in OUTCOMES:
            raise ValueError(f"invalid outcome {row['outcome']} in {path}")
        result[query_id] = row
    return result


def accepted(row: dict[str, str]) -> bool:
    return (
        row["termination"] == "completed"
        and row["post_fail"] == "0"
        and row["post_unsupported"] == "0"
    )


def post_invalid(row: dict[str, str]) -> bool:
    return row["post_fail"] != "0" or row["post_unsupported"] != "0"


def coverage(row: dict[str, str]) -> tuple[str, ...]:
    return tuple(row[field] for field in COVERAGE)


def coverage_text(row: dict[str, str]) -> str:
    return (
        f"ICFG={row['analyzed_icfg_nodes']}/{row['icfg_nodes']},"
        f"functions={row['analyzed_functions']}/{row['functions']}"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("shards", type=Path, nargs="+")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)

    records: list[tuple[Path, dict[str, str]]] = []
    metadata: list[tuple[Path, str]] = []
    for shard in args.shards:
        for row in read_tsv(shard / "results.tsv"):
            records.append((shard, row))
        metadata.append((shard, (shard / "metadata.txt").read_text(encoding="utf-8")))

    measured = [(shard, row) for shard, row in records if row["phase"] == "measured"]
    groups: dict[tuple[str, str, str], list[tuple[Path, dict[str, str]]]] = defaultdict(list)
    for item in measured:
        row = item[1]
        groups[tuple(row[field] for field in KEY)].append(item)

    summary_rows: list[dict[str, object]] = []
    representative: dict[tuple[str, str, str], tuple[Path, dict[str, str]]] = {}
    for key in sorted(groups):
        items = groups[key]
        completed = [item for item in items if item[1]["termination"] == "completed"]
        valid = [item for item in completed if accepted(item[1])]
        hashes = {item[1]["query_outcome_sha256"] for item in valid}
        identities = {item[1]["query_identity_sha256"] for item in valid}
        coverages = {coverage(item[1]) for item in valid}
        if len(hashes) > 1 or len(identities) > 1 or len(coverages) > 1:
            raise ValueError(f"nondeterministic accepted result for {key}")
        if valid:
            representative[key] = valid[0]
        walls = [value for _, row in completed if (value := number(row, "elapsed_s")) is not None]
        cpus = [
            (number(row, "user_s") or 0.0) + (number(row, "sys_s") or 0.0)
            for _, row in completed
        ]
        rss = [value for _, row in completed if (value := number(row, "rss_kb")) is not None]
        first = valid[0][1] if valid else items[0][1]
        summary_rows.append({
            "program": key[0], "sparsity": key[1], "domain": key[2],
            "attempts": len(items), "completed": len(completed),
            "accepted": len(valid),
            "completion_rate": f"{len(completed) / len(items):.6f}",
            "wall_median_s": f"{statistics.median(walls):.9g}" if walls else "NA",
            "wall_q1_s": f"{percentile(walls, 0.25):.9g}" if walls else "NA",
            "wall_q3_s": f"{percentile(walls, 0.75):.9g}" if walls else "NA",
            "cpu_median_s": f"{statistics.median(cpus):.9g}" if cpus else "NA",
            "rss_median_kb": f"{statistics.median(rss):.9g}" if rss else "NA",
            "queries": first["queries"] if valid else "NA",
            "safe": first["safe"] if valid else "NA",
            "may": first["may"] if valid else "NA",
            "unreachable": first["unreachable"] if valid else "NA",
            "unsupported": first["unsupported"] if valid else "NA",
            "icfg_nodes": first["icfg_nodes"] if valid else "NA",
            "analyzed_icfg_nodes": first["analyzed_icfg_nodes"] if valid else "NA",
            "functions": first["functions"] if valid else "NA",
            "analyzed_functions": first["analyzed_functions"] if valid else "NA",
            "query_identity_sha256": next(iter(identities), "missing"),
            "query_outcome_sha256": next(iter(hashes), "missing"),
        })

    failures = []
    for shard, row in records:
        if row["termination"] != "completed" or row["post_fail"] != "0" or row["post_unsupported"] != "0":
            failures.append({
                "program": row["program"], "phase": row["phase"], "run": row["run"],
                "sparsity": row["sparsity"], "domain": row["domain"],
                "status": row["status"], "termination": row["termination"],
                "elapsed_s": row["elapsed_s"], "post_fail": row["post_fail"],
                "post_unsupported": row["post_unsupported"], "shard": str(shard),
            })

    delta_rows: list[dict[str, object]] = []
    check_rows: list[dict[str, object]] = []
    programs = sorted({key[0] for key in groups})
    for program in programs:
        for sparsity in ("dense", "semi-sparse"):
            baseline_key = (program, sparsity, "box")
            for domain in ("octagon", "polyhedra"):
                candidate_key = (program, sparsity, domain)
                if baseline_key not in representative or candidate_key not in representative:
                    check_rows.append({"program": program, "comparison": f"{sparsity}:box->{domain}",
                                       "status": "incomplete", "detail": "one or both configurations did not complete"})
                    continue
                base_shard, base_row = representative[baseline_key]
                cand_shard, cand_row = representative[candidate_key]
                base = ledger(ledger_path(base_shard, base_row))
                candidate = ledger(ledger_path(cand_shard, cand_row))
                if set(base) != set(candidate):
                    raise ValueError(f"query identity mismatch for {program} {sparsity} {domain}")
                regressions = 0
                for query_id in sorted(base):
                    before = base[query_id]
                    after = candidate[query_id]
                    if before["outcome"] == after["outcome"]:
                        continue
                    transition = f"{before['outcome']}->{after['outcome']}"
                    regressions += (
                        before["outcome"] in ("Safe", "Unreachable")
                        and after["outcome"] in ("May", "Unsupported")
                    )
                    delta_rows.append({
                        "program": program, "sparsity": sparsity, "domain": domain,
                        "query_id": query_id, "detector": before["detector"],
                        "function": before["function"], "source_location": before["source_location"],
                        "query_kind": before["query_kind"], "box_outcome": before["outcome"],
                        "relational_outcome": after["outcome"], "transition": transition,
                        "relational_reason": after["reason"],
                    })
                base_coverage = coverage_text(base_row)
                candidate_coverage = coverage_text(cand_row)
                coverage_matches = coverage(base_row) == coverage(cand_row)
                if not coverage_matches:
                    comparison_status = "coverage-mismatch"
                elif regressions != 0:
                    comparison_status = "regression"
                else:
                    comparison_status = "pass"
                check_rows.append({"program": program, "comparison": f"{sparsity}:box->{domain}",
                                   "status": comparison_status,
                                   "detail": (f"certified-to-alarm regressions={regressions}; "
                                              f"coverage box[{base_coverage}] "
                                              f"candidate[{candidate_coverage}]")})
        for domain in ("box", "octagon", "polyhedra"):
            dense_key = (program, "dense", domain)
            semi_key = (program, "semi-sparse", domain)
            if dense_key not in representative or semi_key not in representative:
                status, detail = "incomplete", "one or both configurations did not complete"
            else:
                dense_shard, dense_row = representative[dense_key]
                semi_shard, semi_row = representative[semi_key]
                dense_ledger = ledger(ledger_path(dense_shard, dense_row))
                semi_ledger = ledger(ledger_path(semi_shard, semi_row))
                identities_match = set(dense_ledger) == set(semi_ledger)
                outcomes_match = (
                    identities_match
                    and all(dense_ledger[key]["outcome"] == semi_ledger[key]["outcome"]
                            for key in dense_ledger)
                )
                coverage_match = coverage(dense_row) == coverage(semi_row)
                if not identities_match:
                    status = "identity-mismatch"
                elif not outcomes_match:
                    status = "mismatch"
                elif not coverage_match:
                    status = "coverage-mismatch"
                else:
                    status = "pass"
                detail = (
                    f"identity={'same' if identities_match else 'different'}; "
                    f"outcomes={'same' if outcomes_match else 'different'}; "
                    f"coverage={'same' if coverage_match else 'different'} "
                    f"dense[{coverage_text(dense_row)}] semi[{coverage_text(semi_row)}]"
                )
            check_rows.append({"program": program, "comparison": f"dense->semi:{domain}",
                               "status": status, "detail": detail})

    cumulative_rows: list[dict[str, object]] = []
    for program in programs + ["__all__"]:
        selected = [row for _, row in records
                    if program == "__all__" or row["program"] == program]
        for phase in ("all", "warmup", "measured"):
            phase_rows = selected if phase == "all" else [row for row in selected if row["phase"] == phase]
            elapsed = [value for row in phase_rows if (value := number(row, "elapsed_s")) is not None]
            cpu = [(number(row, "user_s") or 0.0) + (number(row, "sys_s") or 0.0)
                   for row in phase_rows]
            rss = [value for row in phase_rows if (value := number(row, "rss_kb")) is not None]
            cumulative_rows.append({
                "program": program,
                "phase": phase,
                "attempts": len(phase_rows),
                "completed": sum(row["termination"] == "completed" for row in phase_rows),
                "accepted": sum(accepted(row) for row in phase_rows),
                "post_invalid": sum(post_invalid(row) for row in phase_rows),
                "timeouts": sum(row["termination"] == "timeout" for row in phase_rows),
                "other_failures": sum(
                    row["termination"] not in ("completed", "timeout")
                    and not post_invalid(row)
                    for row in phase_rows
                ),
                "elapsed_sum_s": f"{sum(elapsed):.9g}",
                "cpu_sum_s": f"{sum(cpu):.9g}",
                "peak_rss_kb": f"{max(rss):.9g}" if rss else "NA",
            })

    write_tsv(args.output / "summary.tsv", list(summary_rows[0]) if summary_rows else [], summary_rows)
    write_tsv(args.output / "failures.tsv",
              ["program", "phase", "run", "sparsity", "domain", "status", "termination",
               "elapsed_s", "post_fail", "post_unsupported", "shard"], failures)
    delta_fields = ["program", "sparsity", "domain", "query_id", "detector", "function",
                    "source_location", "query_kind", "box_outcome", "relational_outcome",
                    "transition", "relational_reason"]
    write_tsv(args.output / "query_deltas.tsv", delta_fields, delta_rows)
    write_tsv(args.output / "checks.tsv", ["program", "comparison", "status", "detail"], check_rows)
    write_tsv(args.output / "cumulative_costs.tsv",
              ["program", "phase", "attempts", "completed", "accepted",
               "post_invalid", "timeouts", "other_failures", "elapsed_sum_s",
               "cpu_sum_s", "peak_rss_kb"],
              cumulative_rows)

    digest = hashlib.sha256()
    manifest_lines = []
    for shard, text in metadata:
        digest.update(text.encode())
        manifest_lines.append(f"shard={shard}\n{text.rstrip()}\n")
    manifest_lines.append(f"metadata_sha256={digest.hexdigest()}\n")
    (args.output / "manifest.txt").write_text("\n".join(manifest_lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
