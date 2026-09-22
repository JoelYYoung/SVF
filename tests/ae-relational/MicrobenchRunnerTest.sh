#!/usr/bin/env bash
set -euo pipefail

if (( $# != 2 )); then
  echo "usage: $0 RUNNER FIXTURE" >&2
  exit 2
fi

runner=$1
fixture=$2
if [[ $(uname -s) != Linux ]] && ! command -v gtimeout >/dev/null 2>&1; then
  echo "MicrobenchRunnerTest: SKIP (GNU timeout unavailable)"
  exit 77
fi

temporary_directory=$(mktemp -d)
trap 'rm -rf "$temporary_directory"' EXIT
for behavior in success fail timeout; do
  FIXTURE_BEHAVIOR=$behavior SVF_MICROBENCH_TIMEOUT_SECONDS=1 \
  SVF_MICROBENCH_BACKENDS=native SVF_MICROBENCH_DOMAINS=octagon \
  SVF_MICROBENCH_DIMENSIONS=4 SVF_MICROBENCH_SHAPES=sparse \
    "$runner" "$fixture" "$temporary_directory/$behavior.tsv" 1
done

awk -F '\t' 'NR == 2 {
  if (NF != 11 || $5 != "completed" || $6 != 0 || $9 != 1) exit 1
}' "$temporary_directory/success.cases.tsv"
awk -F '\t' 'NR == 2 {
  if (NF != 11 || $5 != "failed" || $6 != 7 || $9 != 1) exit 1
}' "$temporary_directory/fail.cases.tsv"
awk -F '\t' 'NR == 2 {
  if (NF != 11 || $5 != "timeout" || $6 != 124 || $9 != 1) exit 1
}' "$temporary_directory/timeout.cases.tsv"

read -r fail_stderr fail_timing < <(
  awk -F '\t' 'NR == 2 { print $10, $11 }' \
    "$temporary_directory/fail.cases.tsv")
test "$fail_stderr" = \
  'fail.cases/native-octagon-4-sparse.stderr.txt'
test "$fail_timing" = \
  'fail.cases/native-octagon-4-sparse.time.txt'
grep -F 'fixture failure' "$temporary_directory/$fail_stderr"
grep -F 'fixture timeout' \
  "$temporary_directory/timeout.cases/native-octagon-4-sparse.stderr.txt"
for behavior in success fail timeout; do
  test -s \
    "$temporary_directory/$behavior.cases/native-octagon-4-sparse.time.txt"
done

echo "MicrobenchRunnerTest: PASS"
