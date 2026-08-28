#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <svf-octagon-storage-benchmark> <output.csv>" >&2
  exit 2
fi

benchmark=$1
output=$2
completed=0

"$benchmark" --header >"$output"

run_case() {
  local scheme=$1
  local workload=$2
  local topology=$3
  local variables=$4
  local component_size=$5
  local operations=$6
  local repetitions=$7

  "$benchmark" \
    --scheme "$scheme" \
    --workload "$workload" \
    --topology "$topology" \
    --variables "$variables" \
    --component-size "$component_size" \
    --operations "$operations" \
    --repetitions "$repetitions" \
    --seed 20260828 >>"$output"
  completed=$((completed + 1))
  if (( completed % 20 == 0 )); then
    echo "completed $completed Octagon storage configurations" >&2
  fi
}

schemes=(dense-half sparse-finite component-dense)

# True block-sparse states: no unary anchors, so closure cannot connect blocks.
for variables in 64 256 1024; do
  for component_size in 2 8 32; do
    for scheme in "${schemes[@]}"; do
      run_case "$scheme" lookup block "$variables" "$component_size" 500000 9
      # Copy cost scales with the carrier's allocated representation, from
      # O(n^2) for dense-half to the number of finite/component slots for the
      # sparse carriers.  Keep every timed sample long enough to measure while
      # avoiding billions of unnecessary Bound copies at n=1024.
      copy_operations=1000
      copy_repetitions=9
      if (( variables == 256 )) && [[ "$scheme" == "dense-half" ]]; then
        copy_operations=50
      elif (( variables == 1024 )); then
        copy_repetitions=7
        case "$scheme" in
          dense-half) copy_operations=5 ;;
          sparse-finite) copy_operations=1000 ;;
          component-dense) copy_operations=100 ;;
        esac
      fi
      run_case "$scheme" copy-update block "$variables" "$component_size" \
        "$copy_operations" "$copy_repetitions"
      if (( variables == 64 )); then
        close_operations=20
        close_repetitions=9
      elif (( variables == 256 )); then
        close_operations=5
        close_repetitions=7
      else
        close_operations=1
        close_repetitions=5
      fi
      run_case "$scheme" close block "$variables" "$component_size" \
        "$close_operations" "$close_repetitions"
    done
  done
done

# One representative incremental close after a safe within-component edge.
for scheme in "${schemes[@]}"; do
  run_case "$scheme" incremental-close block 256 8 5 7
done

# Unary anchors are the adversarial topology: strong closure derives cross-block
# bounds, so an exact component carrier must merge the anchored components.
for variables in 32 64 128; do
  for scheme in "${schemes[@]}"; do
    run_case "$scheme" lookup anchored "$variables" 8 500000 9
    run_case "$scheme" close anchored "$variables" 8 2 7
  done
done

echo "completed $completed Octagon storage configurations" >&2
