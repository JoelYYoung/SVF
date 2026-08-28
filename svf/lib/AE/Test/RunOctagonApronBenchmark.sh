#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <svf-octagon-apron-benchmark> <output.csv>" >&2
  exit 2
fi

benchmark=$1
output=$2
"$benchmark" --header >"$output"

for topology in block anchored; do
  for variables in 8 16 32; do
    for implementation in native-octagon apron-octagon; do
      for workload in construct copy-update join; do
        operations=5
        if (( variables == 16 )); then
          operations=2
        elif (( variables == 32 )); then
          operations=1
        fi
        "$benchmark" \
          --implementation "$implementation" \
          --workload "$workload" \
          --topology "$topology" \
          --variables "$variables" \
          --component-size 8 \
          --operations "$operations" \
          --repetitions 5 >>"$output"
      done
    done
  done
done
