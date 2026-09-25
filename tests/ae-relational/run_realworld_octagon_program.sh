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
timeout_seconds=${SVF_AE_REALWORLD_TIMEOUT_SECONDS:-300}
memory_kib=${SVF_AE_REALWORLD_MEMORY_KIB:-33554432}
max_vars=${SVF_AE_RELATIONAL_MAX_VARS:-32}
order_offset=${SVF_AE_REALWORLD_ORDER_OFFSET:-0}
expected_ae_sha=${SVF_AE_EXPECTED_SHA256:-}
expected_extapi_sha=${SVF_AE_EXPECTED_EXTAPI_SHA256:-}

test ! -e "$output_dir" || {
  echo "output already exists: $output_dir" >&2
  exit 3
}
mkdir -p "$output_dir/runs"
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
IFS=$'\t' read -r program_key relative input_id instructions functions \
  campaign_lane historical_role <<< "$row"
input=$dataset_root/$relative
test "$(sha256sum "$input" | cut -d' ' -f1)" = "$input_id" || exit 6
ordinal=$(awk -F '\t' -v program="$program" \
  'NR > 1 { row_index++ } $1 == program { print row_index; exit }' "$cohort")
cpu=$((7 + (ordinal - 1) % 80))

base_configs=(
  'box-dense:dense:box:whole:through'
  'box-semi:semi-sparse:box:whole:through'
  'octagon-dense:dense:octagon:eva-style:through'
  'octagon-semi:semi-sparse:octagon:eva-style:through'
)
configs=()
rotation=$(((ordinal + order_offset) % ${#base_configs[@]}))
for index in 0 1 2 3; do
  configs+=("${base_configs[$(((index + rotation) % ${#base_configs[@]}))]}")
done

results=$output_dir/results.tsv
printf 'program_key\trelative_path\tinput_sha256\tinstructions\tfunctions\tcampaign_lane\thistorical_role\torder\tconfig\tsparsity\tdomain\tpolicy\tcalls\tstatus\ttermination\ttimeout_s\tmemory_kib\telapsed_s\tuser_s\tsys_s\trss_kb\tllvm_ir_s\tsvfir_s\tpta_s\tai_s\tquery_s\tpost_s\ticfg_nodes\tanalyzed_icfg_nodes\tanalyzed_functions\tqueries\tsafe\tmay\tunreachable\tunsupported\tquery_identity_sha256\tquery_outcome_sha256\tpost_pass\tpost_infeasible\tpost_unreachable\tpost_fail\tpost_unsupported\tseeds\trecognized_pairs\tcandidates\tselected\tdropped\tclosure_calls\tclosure_exports\tclosure_indexed\tprojections\n' > "$results"

read_time_field() { sed -n "s/^$1=//p" "$2" | tail -n 1; }
read_stat_field() {
  local value
  value=$(awk -v key="$1" '$1 == key { value=$2 } END { print value }' "$2")
  printf '%s' "${value:-NA}"
}
read_phase_field() {
  local value
  value=$(sed -n "s/.*AE_PHASE_TIMES .*$1=\([^ ]*\).*/\1/p" "$2" | tail -n 1)
  printf '%s' "${value:-NA}"
}
read_named_field() {
  local value
  value=$(sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p" "$2" | tail -n 1)
  printf '%s' "${value:-NA}"
}

run_one()
{
  local order=$1 config=$2 sparsity=$3 domain=$4 policy=$5 call_policy=$6
  local stem=$output_dir/runs/$order-$config
  set +e
  /usr/bin/time -o "$stem.time" \
    -f 'elapsed_s=%e\nuser_s=%U\nsys_s=%S\nrss_kb=%M\ntime_exit=%x' \
    taskset -c "$cpu" timeout --signal=TERM --kill-after=10 "$timeout_seconds" \
      bash -c 'ulimit -v "$1"; shift; exec env SVF_AE_PHASE_STATS=1 SVF_AE_DOMAIN_STATS=1 "$@"' limit \
        "$memory_kib" "$ae_bin" -extapi="$extapi_bc" -ae-backend=native \
        -ae-domain="$domain" -ae-sparsity="$sparsity" \
        -ae-relational-policy="$policy" -ae-relational-calls="$call_policy" \
        -ae-relational-max-vars="$max_vars" -handle-recur=top \
        -model-consts=true -model-arrays=true -pre-field-sensitive=false \
        -stat=true -overflow=true -null-deref=true \
        -ae-query-input-id="$input_id" -ae-query-ledger="$stem.queries.tsv" \
        -ae-post-check="$stem.post.tsv" "$input" \
        > "$stem.stdout" 2> "$stem.stderr"
  local run_status=$?
  set -u

  local termination=failed
  if ((run_status == 0)); then termination=completed
  elif ((run_status == 124)); then termination=timeout
  elif ((run_status == 137)); then termination=limit-or-kill
  elif ((run_status >= 128)); then termination=signal-$((run_status - 128))
  fi

  local queries=0 safe=0 may=0 unreachable=0 unsupported=0
  local query_identity=missing query_outcome=missing
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
  local post_pass=0 post_infeasible=0 post_unreachable=0 post_fail=0
  local post_unsupported=0
  if [[ -f $stem.post.tsv ]]; then
    read -r post_pass post_infeasible post_unreachable post_fail \
      post_unsupported < <(
      awk -F '\t' 'NR > 1 { count[$6]++ }
        END { print count["Pass"]+0, count["Infeasible"]+0,
          count["Unreachable"]+0, count["Fail"]+0,
          count["Unsupported"]+0 }' "$stem.post.tsv")
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$program_key" "$relative" "$input_id" "$instructions" "$functions" \
    "$campaign_lane" "$historical_role" "$order" "$config" "$sparsity" \
    "$domain" "$policy" "$call_policy" "$run_status" "$termination" \
    "$timeout_seconds" "$memory_kib" \
    "$(read_time_field elapsed_s "$stem.time")" \
    "$(read_time_field user_s "$stem.time")" \
    "$(read_time_field sys_s "$stem.time")" \
    "$(read_time_field rss_kb "$stem.time")" \
    "$(read_stat_field LLVMIRTime "$stem.stdout")" \
    "$(read_stat_field SVFIRTime "$stem.stdout")" \
    "$(read_stat_field TotalTime "$stem.stdout")" \
    "$(read_phase_field ai_s "$stem.stdout")" \
    "$(read_phase_field query_s "$stem.stdout")" \
    "$(read_phase_field post_s "$stem.stdout")" \
    "$(read_stat_field ICFG_Node_Num "$stem.stdout")" \
    "$(read_stat_field Analyzed_ICFG_Node_Num "$stem.stdout")" \
    "$(read_stat_field Analyzed_Func_Num "$stem.stdout")" \
    "$queries" "$safe" "$may" "$unreachable" "$unsupported" \
    "$query_identity" "$query_outcome" "$post_pass" "$post_infeasible" \
    "$post_unreachable" "$post_fail" "$post_unsupported" \
    "$(read_named_field seeds "$stem.stdout")" \
    "$(read_named_field recognized_pairs "$stem.stdout")" \
    "$(read_named_field candidates "$stem.stdout")" \
    "$(read_named_field selected "$stem.stdout")" \
    "$(read_named_field dropped "$stem.stdout")" \
    "$(read_named_field closure_calls "$stem.stdout")" \
    "$(read_named_field closure_exports "$stem.stdout")" \
    "$(read_named_field closure_indexed "$stem.stdout")" \
    "$(read_named_field projections "$stem.stdout")" >> "$results"
}

for order in 0 1 2 3; do
  IFS=: read -r config sparsity domain policy call_policy <<< "${configs[$order]}"
  run_one "$order" "$config" "$sparsity" "$domain" "$policy" "$call_policy"
done

gate_status=0
gate_reason=capacity-incomplete
test "$(($(wc -l < "$results") - 1))" -eq 4 || {
  gate_status=1
  gate_reason=row-count
}
completed=$(awk -F '\t' 'NR > 1 && $15 == "completed" { count++ } END { print count+0 }' "$results")
# A Post failure makes AE exit nonzero, so that row is not classified as
# "completed". Check every emitted row before applying capacity rules;
# otherwise a semantic failure is mislabeled as an ordinary incomplete run.
awk -F '\t' 'NR > 1 &&
    ($15 == "failed" || $15 ~ /^signal-/ || $41 != 0 || $42 != 0) { bad++ }
  END { exit bad != 0 }' "$results" || {
  gate_status=1
  gate_reason=post-or-status
}
if ((completed >= 2)); then
  identities=$(awk -F '\t' '$15 == "completed" { print $36 }' "$results" | \
    LC_ALL=C sort -u | wc -l)
  if ((identities != 1)); then
    gate_status=1
    gate_reason=query-identity
  fi
fi
if ((completed == 4 && gate_status == 0)); then gate_reason=passed; fi

printf 'program_key=%s\ninput_sha256=%s\ncohort_sha256=%s\nrunner_sha256=%s\nae_sha256=%s\nextapi_sha256=%s\ntimeout_seconds=%s\nmemory_kib=%s\nmax_vars=%s\norder_offset=%s\ncpu=%s\nhost=%s\ncompleted_configs=%s\ngate_status=%s\ngate_reason=%s\ncompleted_at=%s\n' \
  "$program_key" "$input_id" "$(sha256sum "$cohort" | cut -d' ' -f1)" \
  "$(sha256sum "$0" | cut -d' ' -f1)" "$(sha256sum "$ae_bin" | cut -d' ' -f1)" \
  "$(sha256sum "$extapi_bc" | cut -d' ' -f1)" "$timeout_seconds" \
  "$memory_kib" "$max_vars" "$order_offset" "$cpu" "$(hostname)" "$completed" \
  "$gate_status" "$gate_reason" "$(date -u +%FT%TZ)" \
  > "$output_dir/manifest.txt"
find "$output_dir" -type f ! -name sha256.txt -print0 | sort -z | \
  xargs -0 sha256sum > "$output_dir/sha256.txt"
cat "$output_dir/manifest.txt"
cat "$results"
exit "$gate_status"
