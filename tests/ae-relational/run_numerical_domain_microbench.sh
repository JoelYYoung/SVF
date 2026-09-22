#!/usr/bin/env bash
set -uo pipefail

if (( $# < 2 || $# > 3 )); then
  echo "usage: $0 BENCHMARK_BIN OUTPUT_TSV [ITERATIONS]" >&2
  exit 2
fi

benchmark_bin=$1
output_tsv=$2
iterations=${3:-1}
timeout_seconds=${SVF_MICROBENCH_TIMEOUT_SECONDS:-300}
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
if [[ ! $timeout_seconds =~ ^[1-9][0-9]*$ ]]; then
  echo "SVF_MICROBENCH_TIMEOUT_SECONDS must be a positive integer" >&2
  exit 2
fi
if [[ $(uname -s) != Linux ]] && ! command -v gtimeout >/dev/null 2>&1; then
  echo "GNU gtimeout is required on non-Linux hosts" >&2
  exit 2
fi

mkdir -p "$(dirname "$output_tsv")"
temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT
case_tsv="${output_tsv%.tsv}.cases.tsv"
case_artifact_directory="${output_tsv%.tsv}.cases"
mkdir -p "$case_artifact_directory"

if ! "$benchmark_bin" --header |
    awk '{ print $0 "\tpeak_rss_kb" }' > "$output_tsv"; then
  echo "failed to read benchmark TSV header" >&2
  exit 1
fi
printf 'backend\tdomain\tdimension\tshape\tstatus\texit\telapsed_s\tpeak_rss_kb\toperation_rows\tstderr_path\ttiming_path\n' \
  > "$case_tsv"

attempted_cases=0
expected_cases=$((${#backends[@]} * ${#domains[@]} *
                  ${#dimensions[@]} * ${#shapes[@]}))

run_case()
{
  local backend=$1
  local domain=$2
  local dimension=$3
  local shape=$4
  local rows="$temporary_directory/rows.tsv"
  local timing="$temporary_directory/time.txt"
  local stderr_file="$temporary_directory/stderr.txt"
  local artifact_name="$backend-$domain-$dimension-$shape"
  local artifact_stem="$case_artifact_directory/$artifact_name"
  local saved_stderr="$artifact_stem.stderr.txt"
  local saved_timing="$artifact_stem.time.txt"
  local recorded_stderr="${case_artifact_directory##*/}/$artifact_name.stderr.txt"
  local recorded_timing="${case_artifact_directory##*/}/$artifact_name.time.txt"
  local -a command=(
    "$benchmark_bin"
    --backend "$backend" --domain "$domain" --dimension "$dimension"
    --shape "$shape" --iterations "$iterations"
  )
  local exit_code elapsed_s rss_kb status operation_rows

  : > "$rows"
  : > "$timing"
  : > "$stderr_file"
  attempted_cases=$((attempted_cases + 1))

  if [[ $(uname -s) == Linux ]]; then
    command=(timeout --signal=TERM --kill-after=5 "$timeout_seconds"
             "${command[@]}")
    /usr/bin/time -f 'elapsed_s=%e\nrss_kb=%M' -o "$timing" \
      "${command[@]}" > "$rows" 2> "$stderr_file"
    exit_code=$?
    elapsed_s=$(sed -n 's/^elapsed_s=//p' "$timing" | tail -n 1)
    rss_kb=$(sed -n 's/^rss_kb=//p' "$timing" | tail -n 1)
  else
    command=(gtimeout --signal=TERM --kill-after=5 "$timeout_seconds"
             "${command[@]}")
    /usr/bin/time -l "${command[@]}" > "$rows" 2> "$timing"
    exit_code=$?
    elapsed_s=$(awk 'NR == 1 && $2 == "real" { print $1 }' "$timing")
    rss_kb=$(awk '/maximum resident set size/ {
                    print int(($1 + 1023) / 1024)
                  }' "$timing" | tail -n 1)
  fi

  [[ -n $elapsed_s ]] || elapsed_s=NA
  [[ -n $rss_kb ]] || rss_kb=NA
  operation_rows=$(awk 'NR > 1 { count++ } END { print count + 0 }' "$rows")
  cp "$stderr_file" "$saved_stderr"
  cp "$timing" "$saved_timing"

  if (( exit_code == 0 )); then
    status=completed
  elif (( exit_code == 124 )); then
    status=timeout
  else
    status=failed
  fi

  awk -v rss="$rss_kb" 'NR > 1 { print $0 "\t" rss }' "$rows" >> "$output_tsv"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$backend" "$domain" "$dimension" "$shape" "$status" "$exit_code" \
    "$elapsed_s" "$rss_kb" "$operation_rows" "$recorded_stderr" \
    "$recorded_timing" >> "$case_tsv"
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

if (( attempted_cases != expected_cases )); then
  echo "attempted $attempted_cases cases; expected $expected_cases" >&2
  exit 1
fi
