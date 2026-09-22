#!/usr/bin/env bash
set -uo pipefail

if (($# != 5)); then
  echo "usage: $0 AE_BIN EXTAPI_BC DATASET_ROOT COHORT_TSV OUTPUT_DIR" >&2
  exit 2
fi

ae_bin=$1
extapi_bc=$2
dataset_root=$3
cohort=$4
output_dir=$5
timeout_seconds=${SVF_AE_SCREEN_TIMEOUT_SECONDS:-120}
whole_poly_timeout=${SVF_AE_WHOLE_POLY_TIMEOUT_SECONDS:-30}
memory_kib=${SVF_AE_SCREEN_MEMORY_KIB:-16777216}
cpu=${SVF_AE_SCREEN_CPU:-8}
max_vars=${SVF_AE_RELATIONAL_MAX_VARS:-32}
mkdir -p "$output_dir"

configs=(
  'dense:box:whole' 'semi-sparse:box:whole'
  'dense:octagon:whole' 'semi-sparse:octagon:whole'
  'dense:octagon:query-slice' 'semi-sparse:octagon:query-slice'
  'dense:polyhedra:whole' 'semi-sparse:polyhedra:whole'
  'dense:polyhedra:query-slice' 'semi-sparse:polyhedra:query-slice'
)
results="$output_dir/results.tsv"
metadata="$output_dir/metadata.txt"
printf 'program\tinput_sha256\tsparsity\tdomain\tpolicy\tstatus\ttermination\ttimeout_s\telapsed_s\tuser_s\tsys_s\trss_kb\tllvm_ir_s\tsvfir_s\tpta_s\tai_s\tquery_s\tqueries\tsafe\tmay\tunreachable\tunsupported\tquery_identity_sha256\tquery_outcome_sha256\ticfg_nodes\tanalyzed_icfg_nodes\tfunctions\tanalyzed_functions\tseeds\tselected\tdropped\tinside_ops\tfallback_ops\tprojections\n' > "$results"

{
  printf 'ae=%s\n' "$ae_bin"
  printf 'ae_sha256=%s\n' "$(sha256sum "$ae_bin" | cut -d' ' -f1)"
  printf 'extapi=%s\n' "$extapi_bc"
  printf 'extapi_sha256=%s\n' "$(sha256sum "$extapi_bc" | cut -d' ' -f1)"
  printf 'dataset_root=%s\ncohort=%s\n' "$dataset_root" "$cohort"
  printf 'cohort_sha256=%s\n' "$(sha256sum "$cohort" | cut -d' ' -f1)"
  printf 'timeout_seconds=%s\nwhole_poly_timeout_seconds=%s\n' \
    "$timeout_seconds" "$whole_poly_timeout"
  printf 'memory_kib=%s\ncpu=%s\nmax_vars=%s\n' \
    "$memory_kib" "$cpu" "$max_vars"
  printf 'host=%s\nuname=%s\n' "$(hostname)" "$(uname -a)"
} > "$metadata"

read_time_field()
{
  sed -n "s/^$1=//p" "$2" | tail -n 1
}

read_stat_field()
{
  awk -v key="$1" '$1 == key { print $2 }' "$2" | tail -n 1
}

read_phase_field()
{
  sed -n "s/.*AE_PHASE_TIMES .*$1=\([^ ]*\).*/\1/p" "$2" | tail -n 1
}

read_named_field()
{
  sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p" "$2" | tail -n 1
}

run_one()
{
  program=$1
  relative=$2
  input_id=$3
  sparsity=$4
  domain=$5
  policy=$6
  limit=$timeout_seconds
  if [[ $domain == polyhedra && $policy == whole ]]; then
    limit=$whole_poly_timeout
  fi
  stem="$output_dir/$program-$sparsity-$domain-$policy"
  query="$stem.queries.tsv"
  /usr/bin/time -o "$stem.time" \
    -f 'elapsed_s=%e\nuser_s=%U\nsys_s=%S\nrss_kb=%M\ntime_exit=%x' \
    taskset -c "$cpu" timeout --signal=TERM --kill-after=5 "$limit" \
      bash -c 'ulimit -v "$1"; shift; exec env SVF_AE_PHASE_STATS=1 SVF_AE_DOMAIN_STATS=1 "$@"' screen-limit \
        "$memory_kib" "$ae_bin" -extapi="$extapi_bc" \
        -ae-domain="$domain" -ae-sparsity="$sparsity" \
        -ae-relational-policy="$policy" \
        -ae-relational-max-vars="$max_vars" -handle-recur=top \
        -model-consts=true -model-arrays=true -pre-field-sensitive=false \
        -stat=true -overflow=true -null-deref=true \
        -ae-query-input-id="$input_id" -ae-query-ledger="$query" \
        "$dataset_root/$relative" > "$stem.stdout" 2> "$stem.stderr"
  status=$?
  termination=failed
  if ((status == 0)); then termination=completed
  elif ((status == 124)); then termination=timeout
  elif ((status == 137)); then termination=limit-or-kill
  fi

  queries=0; safe=0; may=0; unreachable=0; unsupported=0
  query_identity=missing; query_outcome=missing
  if [[ -f $query ]]; then
    read -r queries safe may unreachable unsupported < <(
      awk -F '\t' 'NR > 1 { count[$9]++; total++ }
        END { printf "%d %d %d %d %d\n", total+0, count["Safe"]+0,
          count["May"]+0, count["Unreachable"]+0,
          count["Unsupported"]+0 }' "$query")
    query_identity=$(tail -n +2 "$query" | cut -f1 | LC_ALL=C sort |
                     sha256sum | cut -d' ' -f1)
    query_outcome=$(tail -n +2 "$query" | cut -f1,9 | LC_ALL=C sort |
                    sha256sum | cut -d' ' -f1)
  fi

  elapsed=$(read_time_field elapsed_s "$stem.time")
  user=$(read_time_field user_s "$stem.time")
  sys=$(read_time_field sys_s "$stem.time")
  rss=$(read_time_field rss_kb "$stem.time")
  llvm_ir=$(read_stat_field LLVMIRTime "$stem.stdout")
  svfir=$(read_stat_field SVFIRTime "$stem.stdout")
  pta=$(read_stat_field TotalTime "$stem.stdout")
  ai=$(read_phase_field ai_s "$stem.stdout")
  query_time=$(read_phase_field query_s "$stem.stdout")
  icfg=$(read_stat_field ICFG_Node_Num "$stem.stdout")
  analyzed_icfg=$(read_stat_field Analyzed_ICFG_Node_Num "$stem.stdout")
  functions=$(read_stat_field Func_Num "$stem.stdout")
  analyzed_functions=$(read_stat_field Analyzed_Func_Num "$stem.stdout")
  seeds=$(read_named_field seeds "$stem.stdout")
  selected=$(read_named_field selected "$stem.stdout")
  dropped=$(read_named_field dropped "$stem.stdout")
  inside=$(read_named_field inside_ops "$stem.stdout")
  fallback=$(read_named_field fallback_ops "$stem.stdout")
  projections=$(read_named_field projections "$stem.stdout")

  printf '%s\t%s\t%s\t%s\t%s\t%d\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$program" "$input_id" "$sparsity" "$domain" "$policy" "$status" \
    "$termination" "$limit" "${elapsed:-NA}" "${user:-NA}" "${sys:-NA}" \
    "${rss:-NA}" "${llvm_ir:-NA}" "${svfir:-NA}" "${pta:-NA}" \
    "${ai:-NA}" "${query_time:-NA}" "$queries" "$safe" "$may" \
    "$unreachable" "$unsupported" "$query_identity" "$query_outcome" \
    "${icfg:-NA}" "${analyzed_icfg:-NA}" "${functions:-NA}" \
    "${analyzed_functions:-NA}" "${seeds:-NA}" "${selected:-NA}" \
    "${dropped:-NA}" "${inside:-NA}" "${fallback:-NA}" \
    "${projections:-NA}" >> "$results"
}

mapfile -t cohort_rows < <(tail -n +2 "$cohort")
for row in "${cohort_rows[@]}"; do
  IFS=$'\t' read -r program relative expected instructions functions <<< "$row"
  actual=$(sha256sum "$dataset_root/$relative" | cut -d' ' -f1)
  if [[ $actual != "$expected" ]]; then
    echo "input hash mismatch: $program expected=$expected actual=$actual" >&2
    exit 3
  fi
done

for ((program_index=0; program_index<${#cohort_rows[@]}; ++program_index)); do
  IFS=$'\t' read -r program relative input_id instructions functions \
    <<< "${cohort_rows[$program_index]}"
  rotation=$((program_index % ${#configs[@]}))
  for ((position=0; position<${#configs[@]}; ++position)); do
    config=${configs[$(((position + rotation) % ${#configs[@]}))]}
    IFS=: read -r sparsity domain policy <<< "$config"
    run_one "$program" "$relative" "$input_id" "$sparsity" "$domain" "$policy"
  done
done

expected_rows=$((${#cohort_rows[@]} * ${#configs[@]}))
actual_rows=$(($(wc -l < "$results") - 1))
printf 'expected_rows=%d\nactual_rows=%d\n' "$expected_rows" "$actual_rows" \
  >> "$metadata"
test "$actual_rows" -eq "$expected_rows"
