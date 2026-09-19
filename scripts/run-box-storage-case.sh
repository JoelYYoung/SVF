#!/usr/bin/env bash
set -u -o pipefail

if [[ $# -lt 6 || $# -gt 7 ]]; then
    echo "usage: $0 STUDY_ROOT PROFILE BENCHMARK MODE BITCODE CAP_SECONDS [RESULT_SET]" >&2
    exit 2
fi

study=$1
profile=$2
benchmark=$3
mode=$4
bitcode=$5
cap_seconds=$6
result_set=${7:-$profile}

if [[ ! "$result_set" =~ ^[a-zA-Z0-9._-]+$ ]]; then
    echo "result set must be a single safe path component" >&2
    exit 2
fi

case "$profile" in
    coverage)
        executable="$study/build-box-off/bin/ae"
        extapi="$study/build-box-off/lib/extapi.bc"
        ;;
    telemetry)
        executable="$study/build-box-on/bin/box-storage-observer"
        extapi="$study/build-box-on/lib/extapi.bc"
        ;;
    directory-cow)
        executable="$study/build-directory-cow-off/bin/ae"
        extapi="$study/build-directory-cow-off/lib/extapi.bc"
        ;;
    directory-cow-telemetry)
        executable="$study/build-directory-cow-on/bin/box-storage-observer"
        extapi="$study/build-directory-cow-on/lib/extapi.bc"
        ;;
    *)
        echo "unknown storage experiment profile: $profile" >&2
        exit 2
        ;;
esac

for path in "$executable" "$extapi" "$bitcode"; do
    if [[ ! -f "$path" ]]; then
        echo "missing input: $path" >&2
        exit 2
    fi
done

output="$study/results/$result_set/$mode/$benchmark"
mkdir -p "$output"
if [[ -e "$output/finished" ]]; then
    echo "result already finished: $output" >&2
    exit 2
fi

command=(
    "$executable"
    "-ae-sparsity=$mode"
    -ae-fun-entry=main
    -stat=true
    "-extapi=$extapi"
    "$bitcode"
)
printf '%q ' "${command[@]}" > "$output/command.txt"
printf '\n' >> "$output/command.txt"
sha256sum "$executable" "$extapi" "$bitcode" > "$output/SHA256SUMS"
git -C "$study/box-source" rev-parse HEAD > "$output/source-commit.txt"

started=$(date --iso-8601=seconds)
if [[ "$profile" == telemetry || "$profile" == directory-cow-telemetry ]]; then
    BOX_STORAGE_CENSUS_ONLY=1 /usr/bin/time -v -o "$output/time.txt" \
        timeout --signal=TERM "$cap_seconds" "${command[@]}" \
        > "$output/analysis.log" 2>&1
else
    /usr/bin/time -v -o "$output/time.txt" \
        timeout --signal=TERM "$cap_seconds" "${command[@]}" \
        > "$output/analysis.log" 2>&1
fi
exit_code=$?
finished=$(date --iso-8601=seconds)
printf '%s\n' "$exit_code" > "$output/exit-code.txt"
printf '%s\n' "$started" > "$output/started-at.txt"
printf '%s\n' "$finished" > "$output/finished-at.txt"
touch "$output/finished"

grep -E '^(Func_Coverage_Percent|ICFG_Node_Trace|Total_Time|BOX_STORAGE_)' \
    "$output/analysis.log" || true
grep -E '^\s*(Elapsed \(wall clock\) time|Maximum resident set size)' \
    "$output/time.txt" || true
echo "ANALYSIS_EXIT_CODE $exit_code"
exit "$exit_code"
