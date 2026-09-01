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
    for implementation in \
      native-dense-half \
      native-sparse-finite \
      native-component-dense \
      apron-octagon; do
      for workload in construct copy-update join; do
        case "$workload:$variables" in
          construct:8) operations=50 ;;
          construct:16) operations=20 ;;
          construct:32) operations=5 ;;
          copy-update:8) operations=500 ;;
          copy-update:16) operations=250 ;;
          copy-update:32) operations=100 ;;
          join:8) operations=5000 ;;
          join:16) operations=2000 ;;
          join:32) operations=1000 ;;
        esac
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
