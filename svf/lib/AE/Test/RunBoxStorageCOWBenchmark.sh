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
  local stride=$6

  "$benchmark" \
    --scheme "$scheme" \
    --page-size "$page_size" \
    --workload "$workload" \
    --variables "$variables" \
    --density "$density" \
    --id-stride "$stride" \
    --repetitions 9 >>"$output"
  completed=$((completed + 1))
  if (( completed % 25 == 0 )); then
    echo "completed $completed benchmark configurations" >&2
  fi
}

schemes=(
  dimension-page
  dimension-hash-page
  dimension-radix-page
  variable-page
  variable-hash-page
  variable-radix-page
  variable-hash
  variable-cow-hash
  variable-radix-cow
)
workloads=(read copy-only copy-update environment-change extend-restore resident-fork)

# Common scale/density matrix. Every scheme stores the same exact-rational
# Interval payload so this isolates indexing, sharing, and environment costs.
for variables in 1024 16384 65536; do
  for density in 0.001 0.1 0.5; do
    for workload in "${workloads[@]}"; do
      for scheme in "${schemes[@]}"; do
        run_case "$scheme" 64 "$workload" "$variables" "$density" 8
      done
    done
  done
done

# Direct sorted-vector versus hash-directory comparison across page sizes.
for density in 0.001 0.1; do
  for page_size in 8 64 256; do
    for workload in read copy-update resident-fork; do
      run_case dimension-page "$page_size" "$workload" 65536 "$density" 8
      run_case dimension-hash-page "$page_size" "$workload" 65536 "$density" 8
      run_case dimension-radix-page "$page_size" "$workload" 65536 "$density" 8
    done
  done
done

# VarID-gap sensitivity for the three direct-VarID candidates.
for stride in 1 8 64; do
  for workload in read copy-update environment-change; do
    for scheme in variable-page variable-hash-page variable-radix-page variable-hash variable-cow-hash variable-radix-cow; do
      run_case "$scheme" 64 "$workload" 65536 0.01 "$stride"
    done
  done
done

echo "completed $completed benchmark configurations" >&2
