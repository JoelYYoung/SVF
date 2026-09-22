#!/usr/bin/env bash
set -euo pipefail

if (( $# < 2 || $# > 3 )); then
  echo "usage: $0 BENCHMARK_BIN OUTPUT_TSV [ITERATIONS]" >&2
  exit 2
fi

benchmark_bin=$1
output_tsv=$2
iterations=${3:-1}
dimensions=(4 8 16 32 64 128 256)
shapes=(sparse dense degenerate)
backends=(native elina)
domains=(octagon polyhedra)

if [[ -n ${SVF_MICROBENCH_DIMENSIONS:-} ]]; then
  read -r -a dimensions <<< "$SVF_MICROBENCH_DIMENSIONS"
fi
if [[ -n ${SVF_MICROBENCH_SHAPES:-} ]]; then
  read -r -a shapes <<< "$SVF_MICROBENCH_SHAPES"
fi
if [[ -n ${SVF_MICROBENCH_BACKENDS:-} ]]; then
  read -r -a backends <<< "$SVF_MICROBENCH_BACKENDS"
fi
if [[ -n ${SVF_MICROBENCH_DOMAINS:-} ]]; then
  read -r -a domains <<< "$SVF_MICROBENCH_DOMAINS"
fi

if [[ ! -x $benchmark_bin ]]; then
  echo "benchmark executable is not executable: $benchmark_bin" >&2
  exit 2
fi
if [[ ! $iterations =~ ^[1-9][0-9]*$ ]]; then
  echo "iterations must be a positive integer" >&2
  exit 2
fi

mkdir -p "$(dirname "$output_tsv")"
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT

"$benchmark_bin" --header | awk '{ print $0 "\tpeak_rss_kb" }' > "$output_tsv"

run_case()
{
  local backend=$1
  local domain=$2
  local dimension=$3
  local shape=$4
  local rows="$temporary_directory/rows.tsv"
  local timing="$temporary_directory/time.txt"
  local rss_kb

  if [[ $(uname -s) == Darwin ]]; then
    /usr/bin/time -l "$benchmark_bin" \
      --backend "$backend" --domain "$domain" --dimension "$dimension" \
      --shape "$shape" --iterations "$iterations" > "$rows" 2> "$timing"
    rss_kb=$(awk '/maximum resident set size/ { print int(($1 + 1023) / 1024) }' \
      "$timing" | tail -n 1)
  else
    /usr/bin/time -f '%M' -o "$timing" "$benchmark_bin" \
      --backend "$backend" --domain "$domain" --dimension "$dimension" \
      --shape "$shape" --iterations "$iterations" > "$rows"
    rss_kb=$(tail -n 1 "$timing")
  fi
  [[ -n $rss_kb ]] || rss_kb=NA
  awk -v rss="$rss_kb" 'NR > 1 { print $0 "\t" rss }' "$rows" >> "$output_tsv"
}

for backend in "${backends[@]}"; do
  for domain in "${domains[@]}"; do
    for dimension in "${dimensions[@]}"; do
      for shape in "${shapes[@]}"; do
        run_case "$backend" "$domain" "$dimension" "$shape"
      done
    done
  done
done
