#!/usr/bin/env bash
set -euo pipefail

if (($# != 8)); then
  echo "usage: $0 SINGLE_RUNNER AE_BIN EXTAPI_BC DATASET_ROOT COHORT_TSV PROGRAM BASELINE_DIR OUTPUT_DIR" >&2
  exit 2
fi

single_runner=$1
ae_bin=$2
extapi_bc=$3
dataset_root=$4
cohort=$5
program=$6
baseline_dir=$7
output_dir=$8
repetitions=${SVF_AE_S3_REPETITIONS:-5}
timeout_seconds=${SVF_AE_S3_BOX_TIMEOUT_SECONDS:-300}
memory_kib=${SVF_AE_S3_MEMORY_KIB:-33554432}

test "$repetitions" -ge 1
test ! -e "$output_dir" || {
  echo "output already exists: $output_dir" >&2
  exit 3
}
(cd "$baseline_dir" && sha256sum -c sha256.txt >/dev/null)
mkdir -p "$output_dir"

field() {
  local name=$1 file=$2
  awk -F '\t' -v name="$name" '
    NR == 1 { for (index = 1; index <= NF; index++) if ($index == name) column = index; next }
    NR == 2 { print $column; exit }
  ' "$file"
}

baseline=$baseline_dir/results.tsv
test "$(field program_key "$baseline")" = "$program"
test "$(field termination "$baseline")" = completed
test "$(field post_fail "$baseline")" = 0
test "$(field post_unsupported "$baseline")" = 0
expected_identity=$(field query_identity_sha256 "$baseline")
expected_outcome=$(field query_outcome_sha256 "$baseline")

for ordinal in $(seq 0 "$repetitions"); do
  phase=measured
  if ((ordinal == 0)); then phase=warmup; fi
  run_dir=$output_dir/run-$ordinal
  SVF_AE_S3_BOX_TIMEOUT_SECONDS=$timeout_seconds \
    SVF_AE_S3_MEMORY_KIB=$memory_kib \
    "$single_runner" "$ae_bin" "$extapi_bc" "$dataset_root" "$cohort" \
    "$program" "$run_dir"
  result=$run_dir/results.tsv
  test "$(field status "$result")" = 0
  test "$(field termination "$result")" = completed
  test "$(field query_identity_sha256 "$result")" = "$expected_identity"
  test "$(field query_outcome_sha256 "$result")" = "$expected_outcome"
  test "$(field post_fail "$result")" = 0
  test "$(field post_unsupported "$result")" = 0
  if ((ordinal == 0)); then
    printf 'phase\trepetition\t' > "$output_dir/results.tsv"
    head -n 1 "$result" >> "$output_dir/results.tsv"
  fi
  printf '%s\t%s\t' "$phase" "$ordinal" >> "$output_dir/results.tsv"
  tail -n 1 "$result" >> "$output_dir/results.tsv"
done

printf 'program_key=%s\nrepetitions=%s\ntimeout_seconds=%s\nmemory_kib=%s\nbaseline_identity_sha256=%s\nbaseline_outcome_sha256=%s\nsingle_runner_sha256=%s\nrepeat_runner_sha256=%s\nae_sha256=%s\nextapi_sha256=%s\ncohort_sha256=%s\nhost=%s\ncompleted_at=%s\n' \
  "$program" "$repetitions" "$timeout_seconds" "$memory_kib" \
  "$expected_identity" "$expected_outcome" \
  "$(sha256sum "$single_runner" | cut -d' ' -f1)" \
  "$(sha256sum "$0" | cut -d' ' -f1)" \
  "$(sha256sum "$ae_bin" | cut -d' ' -f1)" \
  "$(sha256sum "$extapi_bc" | cut -d' ' -f1)" \
  "$(sha256sum "$cohort" | cut -d' ' -f1)" \
  "$(hostname)" "$(date -u +%FT%TZ)" > "$output_dir/manifest.txt"
find "$output_dir" -type f ! -name sha256.txt -print0 | sort -z | \
  xargs -0 sha256sum > "$output_dir/sha256.txt"
cat "$output_dir/manifest.txt"
cat "$output_dir/results.tsv"
