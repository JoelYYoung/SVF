#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <svf-box-storage-benchmark> <output.csv>" >&2
  exit 2
fi

benchmark=$1
output=$2
completed=0

"$benchmark" --header >"$output"

run_case() {
  local scheme=$1
  local page_size=$2
  local workload=$3
  local variables=$4
  local density=$5

  "$benchmark" \
    --scheme "$scheme" \
    --page-size "$page_size" \
    --workload "$workload" \
    --variables "$variables" \
    --density "$density" \
    --id-stride 8 \
    --repetitions 9 >>"$output"
  completed=$((completed + 1))
  if (( completed % 25 == 0 )); then
    echo "completed $completed benchmark configurations" >&2
  fi
}

schemes=(
  dimension-page
  dimension-cow-page
  dimension-hash-page
  dimension-cow-hash-page
  dimension-radix-page
  variable-page
  variable-hash-page
  variable-radix-page
  variable-hash
  variable-cow-hash
  variable-radix-cow
)
workloads=(forget-reinsert join-scan subset-scan)

# The dimensions and densities bracket the observed production carriers:
# c-ares/full-sparse flow states are extremely sparse, whereas the c-blosc2
# scalar carrier and dense states are moderately dense.
for variables in 2048 8192; do
  for density in 0.001 0.1 0.5; do
    for workload in "${workloads[@]}"; do
      for scheme in "${schemes[@]}"; do
        run_case "$scheme" 64 "$workload" "$variables" "$density"
      done
    done
  done
done

# Page-size sensitivity for the three dimension-keyed directories. Direct
# VarID layouts are covered at page 64 above because their gap sensitivity was
# already isolated by RunBoxStorageCOWBenchmark.sh.
for density in 0.001 0.1; do
  for page_size in 8 16 64; do
    for workload in "${workloads[@]}"; do
      run_case dimension-page "$page_size" "$workload" 8192 "$density"
      run_case dimension-hash-page "$page_size" "$workload" 8192 "$density"
      run_case dimension-radix-page "$page_size" "$workload" 8192 "$density"
    done
  done
done

echo "completed $completed benchmark configurations" >&2
