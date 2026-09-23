#!/usr/bin/env bash
set -uo pipefail

if (($# != 6)); then
  echo "usage: $0 AE_BIN EXTAPI_BC DATASET_ROOT COHORT_TSV PROGRAM OUTPUT_DIR" >&2
  exit 2
fi

ae_bin=$1
extapi_bc=$2
dataset_root=$3
cohort=$4
program=$5
output_dir=$6
timeout_seconds=${SVF_AE_S3_BOX_TIMEOUT_SECONDS:-300}
memory_kib=${SVF_AE_S3_MEMORY_KIB:-33554432}
expected_ae_sha=${SVF_AE_EXPECTED_SHA256:-}
expected_extapi_sha=${SVF_AE_EXPECTED_EXTAPI_SHA256:-}

test ! -e "$output_dir" || {
  echo "output already exists: $output_dir" >&2
  exit 3
}
mkdir -p "$output_dir"
if [[ -n $expected_ae_sha ]]; then
  test "$(sha256sum "$ae_bin" | cut -d' ' -f1)" = "$expected_ae_sha" || exit 4
fi
if [[ -n $expected_extapi_sha ]]; then
  test "$(sha256sum "$extapi_bc" | cut -d' ' -f1)" = "$expected_extapi_sha" || exit 4
fi

row=$(awk -F '\t' -v program="$program" 'NR > 1 && $1 == program { print; exit }' "$cohort")
[[ -n $row ]] || {
  echo "program absent from cohort: $program" >&2
  exit 5
}
IFS=$'\t' read -r program_key relative input_id bytes instructions functions \
  basic_blocks calls indirect_calls loops max_loop_depth has_main \
  direct_main_functions direct_main_instructions static_band band_rank \
  band_size selection_slot s3_role <<< "$row"
input=$dataset_root/$relative
test "$(sha256sum "$input" | cut -d' ' -f1)" = "$input_id" || exit 6
ordinal=$(awk -F '\t' -v program="$program" \
  'NR > 1 { row_index++ } $1 == program { print row_index; exit }' "$cohort")
cpu=$((7 + (ordinal - 1) % 80))
stem=$output_dir/run

set +e
/usr/bin/time -o "$stem.time" \
  -f 'elapsed_s=%e\nuser_s=%U\nsys_s=%S\nrss_kb=%M\ntime_exit=%x' \
  taskset -c "$cpu" timeout --signal=TERM --kill-after=10 "$timeout_seconds" \
    bash -c 'ulimit -v "$1"; shift; exec env SVF_AE_PHASE_STATS=1 "$@"' limit \
      "$memory_kib" "$ae_bin" -extapi="$extapi_bc" -ae-backend=native \
      -ae-domain=box -ae-sparsity=dense -ae-relational-policy=whole \
      -ae-relational-calls=through -ae-relational-max-vars=32 \
      -handle-recur=top -model-consts=true -model-arrays=true \
      -pre-field-sensitive=false -stat=true -overflow=true -null-deref=true \
      -ae-query-input-id="$input_id" -ae-query-ledger="$stem.queries.tsv" \
      -ae-post-check="$stem.post.tsv" "$input" \
      > "$stem.stdout" 2> "$stem.stderr"
run_status=$?
set -e

termination=failed
if ((run_status == 0)); then termination=completed
elif ((run_status == 124)); then termination=timeout
elif ((run_status == 137)); then termination=limit-or-kill
elif ((run_status >= 128)); then termination=signal-$((run_status - 128))
fi

read_time_field() { sed -n "s/^$1=//p" "$stem.time" | tail -n 1; }
read_stat_field() {
  local value
  value=$(awk -v key="$1" '$1 == key { value=$2 } END { print value }' "$stem.stdout")
  printf '%s' "${value:-NA}"
}
read_phase_field() {
  local value
  value=$(sed -n "s/.*AE_PHASE_TIMES .*$1=\([^ ]*\).*/\1/p" "$stem.stdout" | tail -n 1)
  printf '%s' "${value:-NA}"
}

queries=0; safe=0; may=0; unreachable=0; unsupported=0
query_identity=missing; query_outcome=missing
if [[ -f $stem.queries.tsv ]]; then
  read -r queries safe may unreachable unsupported < <(
    awk -F '\t' 'NR > 1 { count[$9]++; total++ }
      END { print total+0, count["Safe"]+0, count["May"]+0,
        count["Unreachable"]+0, count["Unsupported"]+0 }' "$stem.queries.tsv")
  query_identity=$(tail -n +2 "$stem.queries.tsv" | cut -f1 | \
    LC_ALL=C sort | sha256sum | cut -d' ' -f1)
  query_outcome=$(tail -n +2 "$stem.queries.tsv" | cut -f1,9 | \
    LC_ALL=C sort | sha256sum | cut -d' ' -f1)
fi

post_pass=0; post_infeasible=0; post_unreachable=0; post_fail=0
post_unsupported=0
if [[ -f $stem.post.tsv ]]; then
  read -r post_pass post_infeasible post_unreachable post_fail \
    post_unsupported < <(
    awk -F '\t' 'NR > 1 { count[$6]++ }
      END { print count["Pass"]+0, count["Infeasible"]+0,
        count["Unreachable"]+0, count["Fail"]+0,
        count["Unsupported"]+0 }' "$stem.post.tsv")
fi

printf 'program_key\trelative_path\tinput_sha256\tstatic_band\ts3_role\tinstructions\tfunctions\tbasic_blocks\tloops\tindirect_calls\tstatus\ttermination\ttimeout_s\tmemory_kib\telapsed_s\tuser_s\tsys_s\trss_kb\tllvm_ir_s\tsvfir_s\tpta_s\tai_s\tquery_s\tpost_s\ticfg_nodes\tanalyzed_icfg_nodes\tanalyzed_functions\tqueries\tsafe\tmay\tunreachable\tunsupported\tquery_identity_sha256\tquery_outcome_sha256\tpost_pass\tpost_infeasible\tpost_unreachable\tpost_fail\tpost_unsupported\n' > "$output_dir/results.tsv"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$program_key" "$relative" "$input_id" "$static_band" "$s3_role" \
  "$instructions" "$functions" "$basic_blocks" "$loops" "$indirect_calls" \
  "$run_status" "$termination" "$timeout_seconds" "$memory_kib" \
  "$(read_time_field elapsed_s)" "$(read_time_field user_s)" \
  "$(read_time_field sys_s)" "$(read_time_field rss_kb)" \
  "$(read_stat_field LLVMIRTime)" "$(read_stat_field SVFIRTime)" \
  "$(read_stat_field TotalTime)" "$(read_phase_field ai_s)" \
  "$(read_phase_field query_s)" "$(read_phase_field post_s)" \
  "$(read_stat_field ICFG_Node_Num)" "$(read_stat_field Analyzed_ICFG_Node_Num)" \
  "$(read_stat_field Analyzed_Func_Num)" "$queries" "$safe" "$may" \
  "$unreachable" "$unsupported" "$query_identity" "$query_outcome" \
  "$post_pass" "$post_infeasible" "$post_unreachable" "$post_fail" \
  "$post_unsupported" >> "$output_dir/results.tsv"

printf 'program_key=%s\ninput_sha256=%s\ncohort_sha256=%s\nrunner_sha256=%s\nae_sha256=%s\nextapi_sha256=%s\ntimeout_seconds=%s\nmemory_kib=%s\ncpu=%s\nhost=%s\ncompleted_at=%s\n' \
  "$program_key" "$input_id" "$(sha256sum "$cohort" | cut -d' ' -f1)" \
  "$(sha256sum "$0" | cut -d' ' -f1)" "$(sha256sum "$ae_bin" | cut -d' ' -f1)" \
  "$(sha256sum "$extapi_bc" | cut -d' ' -f1)" "$timeout_seconds" \
  "$memory_kib" "$cpu" "$(hostname)" "$(date -u +%FT%TZ)" \
  > "$output_dir/manifest.txt"
find "$output_dir" -type f ! -name sha256.txt -print0 | sort -z | \
  xargs -0 sha256sum > "$output_dir/sha256.txt"
cat "$output_dir/manifest.txt"
cat "$output_dir/results.tsv"
