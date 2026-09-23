#!/usr/bin/env bash
set -euo pipefail

if (($# != 8)); then
  echo "usage: $0 SINGLE_RUNNER AE_BIN EXTAPI_BC DATASET_ROOT COHORT_TSV PROGRAM SCREEN_DIR OUTPUT_DIR" >&2
  exit 2
fi

single_runner=$1
ae_bin=$2
extapi_bc=$3
dataset_root=$4
cohort=$5
program=$6
screen_dir=$7
output_dir=$8
repetitions=${SVF_AE_S3_REPETITIONS:-5}
timeout_seconds=${SVF_AE_S3_PAIR_TIMEOUT_SECONDS:-300}
memory_kib=${SVF_AE_S3_MEMORY_KIB:-33554432}

test "$repetitions" -ge 1
test ! -e "$output_dir" || {
  echo "output already exists: $output_dir" >&2
  exit 3
}
(cd "$screen_dir" && sha256sum -c sha256.txt >/dev/null)
mkdir -p "$output_dir"

field()
{
  local name=$1 config=$2 file=$3
  awk -F '\t' -v name="$name" -v config="$config" '
    NR == 1 {
      for (position = 1; position <= NF; position++) {
        if ($position == name) column = position
        if ($position == "config") config_column = position
      }
      next
    }
    $config_column == config { print $column; exit }
  ' "$file"
}

screen=$screen_dir/results.tsv
test "$(field program_key box "$screen")" = "$program"
test "$(field program_key octagon "$screen")" = "$program"
for config in box octagon; do
  test "$(field status "$config" "$screen")" = 0
  test "$(field termination "$config" "$screen")" = completed
  test "$(field post_fail "$config" "$screen")" = 0
  test "$(field post_unsupported "$config" "$screen")" = 0
done
expected_identity=$(field query_identity_sha256 box "$screen")
test "$(field query_identity_sha256 octagon "$screen")" = "$expected_identity"
expected_box_outcome=$(field query_outcome_sha256 box "$screen")
expected_octagon_outcome=$(field query_outcome_sha256 octagon "$screen")

for repetition in $(seq 0 "$repetitions"); do
  phase=measured
  if ((repetition == 0)); then phase=warmup; fi
  run_dir=$output_dir/run-$repetition
  SVF_AE_S3_PAIR_TIMEOUT_SECONDS=$timeout_seconds \
    SVF_AE_S3_MEMORY_KIB=$memory_kib \
    SVF_AE_S3_PAIR_ORDER_OFFSET=$repetition \
    "$single_runner" "$ae_bin" "$extapi_bc" "$dataset_root" "$cohort" \
    "$program" "$run_dir"
  result=$run_dir/results.tsv
  for config in box octagon; do
    test "$(field status "$config" "$result")" = 0
    test "$(field termination "$config" "$result")" = completed
    test "$(field query_identity_sha256 "$config" "$result")" = "$expected_identity"
    test "$(field post_fail "$config" "$result")" = 0
    test "$(field post_unsupported "$config" "$result")" = 0
  done
  test "$(field query_outcome_sha256 box "$result")" = "$expected_box_outcome"
  test "$(field query_outcome_sha256 octagon "$result")" = "$expected_octagon_outcome"
  if ((repetition == 0)); then
    printf 'phase\trepetition\t' > "$output_dir/results.tsv"
    head -n 1 "$result" >> "$output_dir/results.tsv"
  fi
  while IFS= read -r row; do
    printf '%s\t%s\t%s\n' "$phase" "$repetition" "$row" \
      >> "$output_dir/results.tsv"
  done < <(tail -n +2 "$result")
done

printf 'program_key=%s\nrepetitions=%s\ntimeout_seconds=%s\nmemory_kib=%s\nbaseline_identity_sha256=%s\nbaseline_box_outcome_sha256=%s\nbaseline_octagon_outcome_sha256=%s\nsingle_runner_sha256=%s\nrepeat_runner_sha256=%s\nae_sha256=%s\nextapi_sha256=%s\ncohort_sha256=%s\nhost=%s\ncompleted_at=%s\n' \
  "$program" "$repetitions" "$timeout_seconds" "$memory_kib" \
  "$expected_identity" "$expected_box_outcome" "$expected_octagon_outcome" \
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
