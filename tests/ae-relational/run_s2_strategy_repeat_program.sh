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
screen_root=${SVF_AE_S2_SCREEN_ROOT:?SVF_AE_S2_SCREEN_ROOT is required}
timeout_seconds=${SVF_AE_S2_REPEAT_TIMEOUT_SECONDS:-300}
memory_kib=${SVF_AE_S2_REPEAT_MEMORY_KIB:-16777216}
max_vars=${SVF_AE_RELATIONAL_MAX_VARS:-32}
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
IFS=$'\t' read -r selected_program relative input_id instructions functions <<< "$row"
input=$dataset_root/$relative
test "$(sha256sum "$input" | cut -d' ' -f1)" = "$input_id" || exit 6
ordinal=$(awk -F '\t' -v program="$program" 'NR > 1 { row_index++ } $1 == program { print row_index; exit }' "$cohort")
cpu=$((7 + ordinal))

configs=(
  'dense:box:whole:through'
  'dense:octagon:query-slice:through'
  'dense:octagon:eva-style:through'
  'dense:octagon:query-eva:through'
)

results=$output_dir/results.tsv
printf 'program\tinput_sha256\tinstructions\tfunctions\trepeat\tkind\tposition\tsparsity\tdomain\tpolicy\tcalls\tstatus\ttermination\ttimeout_s\telapsed_s\tuser_s\tsys_s\trss_kb\tllvm_ir_s\tsvfir_s\tpta_s\tai_s\tquery_s\tpost_s\tqueries\tsafe\tmay\tunreachable\tunsupported\tquery_identity_sha256\tquery_outcome_sha256\tpost_pass\tpost_infeasible\tpost_unreachable\tpost_fail\tpost_unsupported\tseeds\trecognized_pairs\tcandidates\tselected\tdropped\tclosure_calls\tclosure_exports\tclosure_indexed\tprojections\n' > "$results"

read_time_field() { sed -n "s/^$1=//p" "$2" | tail -n 1; }
read_stat_field() { awk -v key="$1" '$1 == key { print $2 }' "$2" | tail -n 1; }
read_phase_field() { sed -n "s/.*AE_PHASE_TIMES .*$1=\([^ ]*\).*/\1/p" "$2" | tail -n 1; }
read_named_field() { sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p" "$2" | tail -n 1; }

run_one()
{
  repeat=$1
  kind=$2
  position=$3
  config=$4
  IFS=: read -r sparsity domain policy calls <<< "$config"
  stem=$output_dir/runs/$repeat-$position-$sparsity-$domain-$policy-$calls
  set +e
  /usr/bin/time -o "$stem.time" \
    -f 'elapsed_s=%e\nuser_s=%U\nsys_s=%S\nrss_kb=%M\ntime_exit=%x' \
    taskset -c "$cpu" timeout --signal=TERM --kill-after=5 "$timeout_seconds" \
      bash -c 'ulimit -v "$1"; shift; exec env SVF_AE_PHASE_STATS=1 SVF_AE_DOMAIN_STATS=1 "$@"' limit \
        "$memory_kib" "$ae_bin" -extapi="$extapi_bc" \
        -ae-domain="$domain" -ae-sparsity="$sparsity" \
        -ae-relational-policy="$policy" -ae-relational-calls="$calls" \
        -ae-relational-max-vars="$max_vars" -handle-recur=top \
        -model-consts=true -model-arrays=true -pre-field-sensitive=false \
        -stat=true -overflow=true -null-deref=true \
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

  queries=0; safe=0; may=0; unreachable=0; unsupported=0
  query_identity=missing; query_outcome=missing
  if [[ -f $stem.queries.tsv ]]; then
    read -r queries safe may unreachable unsupported < <(
      awk -F '\t' 'NR > 1 { count[$9]++; total++ }
        END { print total+0, count["Safe"]+0, count["May"]+0,
          count["Unreachable"]+0, count["Unsupported"]+0 }' "$stem.queries.tsv")
    query_identity=$(tail -n +2 "$stem.queries.tsv" | cut -f1 | LC_ALL=C sort | sha256sum | cut -d' ' -f1)
    query_outcome=$(tail -n +2 "$stem.queries.tsv" | cut -f1,9 | LC_ALL=C sort | sha256sum | cut -d' ' -f1)
  fi
  post_pass=0; post_infeasible=0; post_unreachable=0; post_fail=0; post_unsupported=0
  if [[ -f $stem.post.tsv ]]; then
    read -r post_pass post_infeasible post_unreachable post_fail post_unsupported < <(
      awk -F '\t' 'NR > 1 { count[$6]++ }
        END { print count["Pass"]+0, count["Infeasible"]+0,
          count["Unreachable"]+0, count["Fail"]+0,
          count["Unsupported"]+0 }' "$stem.post.tsv")
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$program" "$input_id" "$instructions" "$functions" "$repeat" "$kind" "$position" \
    "$sparsity" "$domain" "$policy" "$calls" "$run_status" "$termination" \
    "$timeout_seconds" "$(read_time_field elapsed_s "$stem.time")" \
    "$(read_time_field user_s "$stem.time")" "$(read_time_field sys_s "$stem.time")" \
    "$(read_time_field rss_kb "$stem.time")" "$(read_stat_field LLVMIRTime "$stem.stdout")" \
    "$(read_stat_field SVFIRTime "$stem.stdout")" "$(read_stat_field TotalTime "$stem.stdout")" \
    "$(read_phase_field ai_s "$stem.stdout")" "$(read_phase_field query_s "$stem.stdout")" \
    "$(read_phase_field post_s "$stem.stdout")" "$queries" "$safe" "$may" \
    "$unreachable" "$unsupported" "$query_identity" "$query_outcome" \
    "$post_pass" "$post_infeasible" "$post_unreachable" "$post_fail" \
    "$post_unsupported" "$(read_named_field seeds "$stem.stdout")" \
    "$(read_named_field recognized_pairs "$stem.stdout")" "$(read_named_field candidates "$stem.stdout")" \
    "$(read_named_field selected "$stem.stdout")" "$(read_named_field dropped "$stem.stdout")" \
    "$(read_named_field closure_calls "$stem.stdout")" "$(read_named_field closure_exports "$stem.stdout")" \
    "$(read_named_field closure_indexed "$stem.stdout")" "$(read_named_field projections "$stem.stdout")" >> "$results"
}

for ((repeat=0; repeat<6; ++repeat)); do
  kind=measured
  ((repeat == 0)) && kind=warmup
  rotation=$(((ordinal + repeat - 1) % ${#configs[@]}))
  for ((position=0; position<${#configs[@]}; ++position)); do
    index=$(((position + rotation) % ${#configs[@]}))
    run_one "$repeat" "$kind" "$position" "${configs[$index]}"
  done
done

gate_status=0
test "$(($(wc -l < "$results") - 1))" -eq 24 || gate_status=1
awk -F '\t' 'NR > 1 && ($12 != 0 || $13 != "completed" || $25 == 0 || $35 != 0 || $36 != 0) { bad++ }
  END { exit bad != 0 }' "$results" || gate_status=1
for config in "${configs[@]}"; do
  IFS=: read -r sparsity domain policy calls <<< "$config"
  screen_row=$(awk -F '\t' -v s="$sparsity" -v d="$domain" -v p="$policy" -v c="$calls" \
    'NR > 1 && $6 == s && $7 == d && $8 == p && $9 == c { print; exit }' "$screen_root/$program/results.tsv")
  [[ -n $screen_row ]] || gate_status=1
  screen_identity=$(cut -f28 <<< "$screen_row")
  screen_outcome=$(cut -f29 <<< "$screen_row")
  awk -F '\t' -v s="$sparsity" -v d="$domain" -v p="$policy" -v c="$calls" \
    -v identity="$screen_identity" -v outcome="$screen_outcome" \
    'NR > 1 && $8 == s && $9 == d && $10 == p && $11 == c && ($30 != identity || $31 != outcome) { bad++ }
     END { exit bad != 0 }' "$results" || gate_status=1
done

printf 'program=%s\ninput_sha256=%s\ncohort_sha256=%s\nrunner_sha256=%s\nae_sha256=%s\nextapi_sha256=%s\ntimeout_seconds=%s\nmemory_kib=%s\nmax_vars=%s\ncpu=%s\nhost=%s\ngate_status=%s\ncompleted_at=%s\n' \
  "$program" "$input_id" "$(sha256sum "$cohort" | cut -d' ' -f1)" \
  "$(sha256sum "$0" | cut -d' ' -f1)" "$(sha256sum "$ae_bin" | cut -d' ' -f1)" \
  "$(sha256sum "$extapi_bc" | cut -d' ' -f1)" "$timeout_seconds" \
  "$memory_kib" "$max_vars" "$cpu" "$(hostname)" "$gate_status" \
  "$(date -u +%FT%TZ)" > "$output_dir/manifest.txt"
find "$output_dir" -type f ! -name sha256.txt -print0 | sort -z | xargs -0 sha256sum > "$output_dir/sha256.txt"
cat "$output_dir/manifest.txt"
cat "$results"
exit "$gate_status"
