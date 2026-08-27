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

schemes=(dimension-page variable-page variable-hash)
workloads=(read copy-update environment-change extend-restore)

# Scale/density block at a moderate ID stride.
for variables in 1024 16384 65536; do
  for density in 0.01 0.1 0.5; do
    for workload in "${workloads[@]}"; do
      for scheme in "${schemes[@]}"; do
        run_case "$scheme" 64 "$workload" "$variables" "$density" 8
      done
    done
  done
done

# Function-sized environments in which almost every logical dimension remains
# top. The resident-fork workload keeps all derived states alive so peak RSS
# includes the COW detaches instead of observing one short-lived copy at a time.
for density in 0.0001 0.001; do
  for stride in 1 8 64; do
    for page_size in 8 16 32 64 128 256; do
      for workload in copy-update resident-fork; do
        run_case dimension-page "$page_size" "$workload" 65536 "$density" "$stride"
        run_case variable-page "$page_size" "$workload" 65536 "$density" "$stride"
      done
    done
  done
  for workload in copy-update resident-fork; do
    run_case variable-hash 64 "$workload" 65536 "$density" 8
  done
done

# ID-gap sensitivity at the largest scale and representative density.
for stride in 1 8 64; do
  for workload in "${workloads[@]}"; do
    for scheme in "${schemes[@]}"; do
      run_case "$scheme" 64 "$workload" 65536 0.1 "$stride"
    done
  done
done

# Page-size tuning for both page-keying strategies.
for density in 0.01 0.1; do
  for stride in 1 8 64; do
    for page_size in 8 16 32 64 128 256; do
      for workload in "${workloads[@]}"; do
        run_case dimension-page "$page_size" "$workload" 65536 "$density" "$stride"
        run_case variable-page "$page_size" "$workload" 65536 "$density" "$stride"
      done
    done
  done
done
