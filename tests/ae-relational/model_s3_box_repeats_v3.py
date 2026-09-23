#!/usr/bin/env python3
"""Refit the rejected S3 Box model using verified measured medians."""

import csv
import json
import math
import pathlib
import statistics
import sys


FEATURES = (
    "instructions",
    "functions",
    "basic_blocks",
    "loops",
    "indirect_calls",
    "direct_main_functions",
    "direct_main_instructions",
)


def linear_fit(xs, ys):
    mean_x = statistics.fmean(xs)
    mean_y = statistics.fmean(ys)
    denominator = sum((value - mean_x) ** 2 for value in xs)
    slope = 0.0 if denominator == 0 else sum(
        (x_value - mean_x) * (y_value - mean_y)
        for x_value, y_value in zip(xs, ys)
    ) / denominator
    return mean_y - slope * mean_x, slope


def predict(coefficients, value):
    intercept, slope = coefficients
    return math.exp(intercept + slope * math.log1p(value))


def balanced_accuracy(labels, predictions):
    positives = sum(labels)
    negatives = len(labels) - positives
    true_positive = sum(actual and predicted for actual, predicted in zip(labels, predictions))
    true_negative = sum(
        (not actual) and (not predicted)
        for actual, predicted in zip(labels, predictions)
    )
    return 0.5 * (true_positive / positives + true_negative / negatives)


def choose_threshold(scores, labels):
    unique = sorted(set(scores))
    candidates = [unique[0] / 2]
    candidates.extend((left + right) / 2 for left, right in zip(unique, unique[1:]))
    candidates.append(unique[-1] * 2)
    ranked = []
    for threshold in candidates:
        predictions = [score <= threshold for score in scores]
        accuracy = balanced_accuracy(labels, predictions)
        ranked.append((-accuracy, threshold, predictions))
    _, threshold, predictions = min(ranked)
    return threshold, balanced_accuracy(labels, predictions)


def load_rows(cohort_path, screen_path):
    with cohort_path.open(newline="") as handle:
        cohort = {
            row["program_key"]: row
            for row in csv.DictReader(handle, delimiter="\t")
        }
    with screen_path.open(newline="") as handle:
        screen = list(csv.DictReader(handle, delimiter="\t"))
    assert set(cohort) == {row["program_key"] for row in screen}
    rows = []
    for row in screen:
        merged = dict(row)
        merged.update(cohort[row["program_key"]])
        for feature in FEATURES:
            merged[feature] = int(merged[feature])
        merged["elapsed_s"] = float(merged["elapsed_s"])
        merged["rss_kb"] = int(merged["rss_kb"])
        merged["valid_for_model"] = merged["valid_for_model"] == "true"
        rows.append(merged)
    return rows


def main():
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: model_s3_box_repeats_v3.py COHORT SCREEN REPEATS OUTPUT"
        )
    cohort_path, screen_path, repeat_path, output_path = map(
        pathlib.Path, sys.argv[1:]
    )
    rows = load_rows(cohort_path, screen_path)
    with repeat_path.open(newline="") as handle:
        repeats = {
            row["program_key"]: row
            for row in csv.DictReader(handle, delimiter="\t")
        }
    assert len(repeats) == 6
    for row in rows:
        repeated = repeats.get(row["program_key"])
        if repeated:
            assert repeated["s3_role"] == row["s3_role"]
            assert repeated["query_identity_sha256"] == row["query_identity_sha256"]
            assert repeated["query_outcome_sha256"] == row["query_outcome_sha256"]
            row["elapsed_s"] = float(repeated["wall_median_s"])
            row["rss_kb"] = int(float(repeated["rss_median_kb"]))

    calibration = [row for row in rows if row["s3_role"] == "calibration"]
    holdout = [row for row in rows if row["s3_role"] == "holdout"]
    completed = [row for row in calibration if row["valid_for_model"]]
    comparable = [
        row for row in calibration
        if row["valid_for_model"] or row["termination"] == "timeout"
    ]
    assert len(completed) == 5
    candidates = []
    for feature in FEATURES:
        xs = [math.log1p(row[feature]) for row in completed]
        ys = [math.log(row["elapsed_s"]) for row in completed]
        coefficients = linear_fit(xs, ys)
        errors = []
        for index, row in enumerate(completed):
            heldout = linear_fit(
                xs[:index] + xs[index + 1:], ys[:index] + ys[index + 1:]
            )
            estimate = predict(heldout, row[feature])
            errors.append(abs(math.log(estimate / row["elapsed_s"])))
        scores = [predict(coefficients, row[feature]) for row in comparable]
        labels = [row["valid_for_model"] for row in comparable]
        threshold, capacity_accuracy = choose_threshold(scores, labels)
        candidates.append({
            "feature": feature,
            "intercept": coefficients[0],
            "slope": coefficients[1],
            "eligible": len(set(xs)) > 1 and abs(coefficients[1]) > 1e-12,
            "loocv_median_multiplicative_error": math.exp(statistics.median(errors)),
            "loocv_max_multiplicative_error": math.exp(max(errors)),
            "calibration_capacity_balanced_accuracy": capacity_accuracy,
            "capacity_threshold_predicted_seconds": threshold,
        })
    selected = min(
        (item for item in candidates if item["eligible"]),
        key=lambda item: (
            -item["calibration_capacity_balanced_accuracy"],
            item["loocv_median_multiplicative_error"],
            item["loocv_max_multiplicative_error"],
            item["feature"],
        ),
    )
    feature = selected["feature"]
    coefficients = (selected["intercept"], selected["slope"])
    threshold = selected["capacity_threshold_predicted_seconds"]
    rss_coefficients = linear_fit(
        [math.log1p(row[feature]) for row in completed],
        [math.log(row["rss_kb"]) for row in completed],
    )
    holdout_comparable = [
        row for row in holdout
        if row["valid_for_model"] or row["termination"] == "timeout"
    ]
    labels = [row["valid_for_model"] for row in holdout_comparable]
    predictions = [
        predict(coefficients, row[feature]) <= threshold
        for row in holdout_comparable
    ]
    holdout_valid = [row for row in holdout if row["valid_for_model"]]
    result = {
        "schema": "svf-s3-box-repeat-model-v3",
        "selection_contract": {
            "candidate_features": list(FEATURES),
            "completion_times": "five-run measured medians for the six valid screen programs",
            "primary": "maximum calibration capacity balanced accuracy",
            "secondary": "minimum calibration-completion LOOCV median multiplicative runtime error",
            "holdout_used_for_selection": False,
            "right_censoring": "300/1800-second timeouts are labels only, never completion times",
        },
        "candidates": candidates,
        "selected": selected,
        "rss_model": {"intercept": rss_coefficients[0], "slope": rss_coefficients[1]},
        "holdout": {
            "rows": len(holdout),
            "comparable_capacity_rows": len(holdout_comparable),
            "true_positive": sum(a and b for a, b in zip(labels, predictions)),
            "false_positive": sum((not a) and b for a, b in zip(labels, predictions)),
            "true_negative": sum((not a) and (not b) for a, b in zip(labels, predictions)),
            "false_negative": sum(a and (not b) for a, b in zip(labels, predictions)),
            "balanced_accuracy": balanced_accuracy(labels, predictions),
            "valid_runtime_rows": len(holdout_valid),
            "runtime_multiplicative_errors": [
                max(
                    predict(coefficients, row[feature]) / row["elapsed_s"],
                    row["elapsed_s"] / predict(coefficients, row[feature]),
                )
                for row in holdout_valid
            ],
            "rss_multiplicative_errors": [
                max(
                    predict(rss_coefficients, row[feature]) / row["rss_kb"],
                    row["rss_kb"] / predict(rss_coefficients, row[feature]),
                )
                for row in holdout_valid
            ],
        },
        "boundary_evidence": {
            "max_valid_program": "gnomekiss__gnomekiss",
            "max_valid_wall_median_s": 166.61,
            "max_valid_analyzed_icfg_nodes": 1905,
            "max_valid_analyzed_functions": 11,
            "selected_1800s_timeouts": [
                "cvsd__cvsd", "ipmitool__ipmitool", "stymulator__ymplayer"
            ],
            "next_7200s_candidate": "cvsd__cvsd",
            "next_candidate_reason": "first-ranked calibration timeout from v2 and smallest total-static input among the three 1800-second timeouts",
        },
        "acceptance": {
            "capacity_model_accepted": False,
            "runtime_model_accepted": False,
            "reason": "Only five valid calibration completions and one valid holdout completion; static-size runtime LOOCV remains unstable. The model is retained only as a documented negative result and selection aid.",
        },
        "limitations": [
            "The split is code-isolated but not analyst-blinded because holdout outcomes were visible before fitting.",
            "Static size is non-monotonic with reachable analysis cost; gnomekiss completes while smaller inputs time out at 1800 seconds.",
            "No single instruction/function cutoff is inferred from this result.",
        ],
    }
    output_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
