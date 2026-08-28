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
  local density=$4
  "$benchmark" --scheme "$scheme" --page-size "$page_size" \
    --workload "$workload" --variables 8192 --density "$density" \
    --id-stride 8 --repetitions 9 >>"$output"
  completed=$((completed + 1))
}

schemes=(
  dimension-page
  dimension-cow-page
  dimension-hash-page
  dimension-cow-hash-page
  dimension-radix-page
)
workloads=(read copy-only copy-update environment-change extend-restore resident-fork)

for density in 0.001 0.1 0.5; do
  for workload in "${workloads[@]}"; do
    for scheme in "${schemes[@]}"; do
      run_case "$scheme" 64 "$workload" "$density"
    done
  done
done

# At very low utilization, payload size can dominate a detach. Isolate that
# effect without multiplying the full experiment matrix.
for page_size in 8 16 64; do
  for workload in copy-update resident-fork; do
    run_case dimension-page "$page_size" "$workload" 0.001
    run_case dimension-cow-page "$page_size" "$workload" 0.001
    run_case dimension-cow-hash-page "$page_size" "$workload" 0.001
  done
done

echo "completed $completed benchmark configurations" >&2
