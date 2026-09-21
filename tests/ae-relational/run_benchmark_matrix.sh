#!/usr/bin/env bash
set -uo pipefail

if (($# != 6)); then
  echo "usage: $0 AE_BIN EXTAPI_BC DATASET_ROOT OUTPUT_DIR MEASURED_RUNS WARMUPS" >&2
  exit 2
fi

ae_bin=$1
extapi_bc=$2
dataset_root=$3
output_dir=$4
measured_runs=$5
warmups=$6
timeout_seconds=${SVF_AE_BENCHMARK_TIMEOUT_SECONDS:-300}
memory_kib=${SVF_AE_BENCHMARK_MEMORY_KIB:-16777216}
cpu=${SVF_AE_BENCHMARK_CPU:-8}

if [[ -n ${SVF_AE_BENCHMARK_PROGRAMS:-} ]]; then
  read -r -a programs <<< "$SVF_AE_BENCHMARK_PROGRAMS"
else
  programs=(
    'cfourcc/cfourcc.bc'
    'libpaper/paperconf.bc'
    'atinout/atinout.bc'
    'flip/flip.bc'
    'bchunk/bchunk.bc'
    'c2050/c2050.bc'
    'dhcping/dhcping.bc'
    'mscompress/mscompress.bc'
  )
fi
configs=(
  'dense:box'
  'dense:octagon'
  'dense:polyhedra'
  'semi-sparse:box'
  'semi-sparse:octagon'
  'semi-sparse:polyhedra'
)

mkdir -p "$output_dir"
results="$output_dir/results.tsv"
metadata="$output_dir/metadata.txt"
printf 'program\tinput_sha256\tphase\trun\tsparsity\tdomain\tstatus\ttermination\telapsed_s\tuser_s\tsys_s\trss_kb\tllvm_ir_s\tsvfir_s\tpta_s\tai_s\tquery_s\tpost_s\tqueries\tsafe\tmay\tunreachable\tunsupported\tquery_identity_sha256\tquery_outcome_sha256\tpost_pass\tpost_infeasible\tpost_unreachable\tpost_fail\tpost_unsupported\ticfg_nodes\tanalyzed_icfg_nodes\tfunctions\tanalyzed_functions\n' > "$results"

{
  printf 'ae=%s\n' "$ae_bin"
  printf 'ae_sha256=%s\n' "$(sha256sum "$ae_bin" | cut -d' ' -f1)"
  printf 'extapi=%s\n' "$extapi_bc"
  printf 'extapi_sha256=%s\n' "$(sha256sum "$extapi_bc" | cut -d' ' -f1)"
  printf 'dataset_root=%s\n' "$dataset_root"
  printf 'timeout_seconds=%s\n' "$timeout_seconds"
  printf 'memory_kib=%s\n' "$memory_kib"
  printf 'cpu=%s\n' "$cpu"
  printf 'measured_runs=%s\n' "$measured_runs"
  printf 'warmups=%s\n' "$warmups"
  printf 'domain_stats=%s\n' "${SVF_AE_DOMAIN_STATS:-0}"
  printf 'uname=%s\n' "$(uname -a)"
  printf 'host=%s\n' "$(hostname)"
  "$ae_bin" --version 2>&1 | sed 's/^/ae_version=/' || true
  for relative in "${programs[@]}"; do
    printf 'input_sha256[%s]=%s\n' "$relative" \
      "$(sha256sum "$dataset_root/$relative" | cut -d' ' -f1)"
  done
} > "$metadata"

read_time_field()
{
  local key=$1
  local file=$2
  sed -n "s/^${key}=//p" "$file" | tail -n 1
}

read_stat_field()
{
  local key=$1
  local file=$2
  awk -v key="$key" '$1 == key { print $2 }' "$file" | tail -n 1
}

read_phase_field()
{
  local key=$1
  local file=$2
  sed -n "s/.*AE_PHASE_TIMES .*${key}=\([^ ]*\).*/\1/p" "$file" |
    tail -n 1
}

run_one()
{
  local relative=$1
  local program=$2
  local input_id=$3
  local phase=$4
  local run=$5
  local sparsity=$6
  local domain=$7
  local stem="$output_dir/${program}-${phase}-r${run}-${sparsity}-${domain}"
  local query="$stem.queries.tsv"
  local post="$stem.post.tsv"

  /usr/bin/time -o "$stem.time" \
    -f 'elapsed_s=%e\nuser_s=%U\nsys_s=%S\nrss_kb=%M\ntime_exit=%x' \
    taskset -c "$cpu" timeout --signal=TERM --kill-after=5 \
      "$timeout_seconds" \
      bash -c 'ulimit -v "$1"; shift; exec env SVF_AE_PHASE_STATS=1 "$@"' benchmark-limit \
        "$memory_kib" "$ae_bin" \
        -extapi="$extapi_bc" \
        -ae-domain="$domain" \
        -ae-sparsity="$sparsity" \
        -handle-recur=top \
        -model-consts=true \
        -model-arrays=true \
        -pre-field-sensitive=false \
        -stat=true \
        -overflow=true \
        -null-deref=true \
        -ae-query-input-id="$input_id" \
        -ae-query-ledger="$query" \
        -ae-post-check="$post" \
        "$dataset_root/$relative" > "$stem.stdout" 2> "$stem.stderr"
  local status=$?
  local termination=failed
  if ((status == 0)); then
    termination=completed
  elif ((status == 124)); then
    termination=timeout
  elif ((status == 137)); then
    termination=limit-or-kill
  fi

  local elapsed user sys rss
  elapsed=$(read_time_field elapsed_s "$stem.time")
  user=$(read_time_field user_s "$stem.time")
  sys=$(read_time_field sys_s "$stem.time")
  rss=$(read_time_field rss_kb "$stem.time")

  local queries=0 safe=0 may=0 unreachable=0 unsupported=0
  local query_identity=missing query_outcome=missing
  if [[ -f $query ]]; then
    read -r queries safe may unreachable unsupported < <(
      awk -F '\t' 'NR > 1 {
        count[$9]++; total++
      } END {
        printf "%d %d %d %d %d\n", total + 0, count["Safe"] + 0,
          count["May"] + 0, count["Unreachable"] + 0,
          count["Unsupported"] + 0
      }' "$query")
    query_identity=$(tail -n +2 "$query" | cut -f1 | LC_ALL=C sort |
                     sha256sum | cut -d' ' -f1)
    query_outcome=$(tail -n +2 "$query" | cut -f1,9 | LC_ALL=C sort |
                    sha256sum | cut -d' ' -f1)
  fi

  local post_pass=0 post_infeasible=0 post_unreachable=0
  local post_fail=0 post_unsupported=0
  if [[ -f $post ]]; then
    read -r post_pass post_infeasible post_unreachable post_fail \
      post_unsupported < <(
      awk -F '\t' 'NR > 1 {
        count[$6]++
      } END {
        printf "%d %d %d %d %d\n", count["Pass"] + 0,
          count["Infeasible"] + 0, count["Unreachable"] + 0,
          count["Fail"] + 0, count["Unsupported"] + 0
      }' "$post")
  fi

  local icfg analyzed_icfg functions analyzed_functions
  icfg=$(read_stat_field ICFG_Node_Num "$stem.stdout")
  analyzed_icfg=$(read_stat_field Analyzed_ICFG_Node_Num "$stem.stdout")
  functions=$(read_stat_field Func_Num "$stem.stdout")
  analyzed_functions=$(read_stat_field Analyzed_Func_Num "$stem.stdout")

  local llvm_ir svfir pta ai query_time post_time
  llvm_ir=$(read_stat_field LLVMIRTime "$stem.stdout")
  svfir=$(read_stat_field SVFIRTime "$stem.stdout")
  pta=$(read_stat_field TotalTime "$stem.stdout")
  ai=$(read_phase_field ai_s "$stem.stdout")
  query_time=$(read_phase_field query_s "$stem.stdout")
  post_time=$(read_phase_field post_s "$stem.stdout")

  printf '%s\t%s\t%s\t%d\t%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%s\t%s\t%s\t%s\n' \
    "$program" "$input_id" "$phase" "$run" "$sparsity" "$domain" \
    "$status" "$termination" "${elapsed:-NA}" "${user:-NA}" \
    "${sys:-NA}" "${rss:-NA}" "${llvm_ir:-NA}" "${svfir:-NA}" \
    "${pta:-NA}" "${ai:-NA}" "${query_time:-NA}" "${post_time:-NA}" \
    "$queries" "$safe" "$may" \
    "$unreachable" "$unsupported" "$query_identity" "$query_outcome" \
    "$post_pass" "$post_infeasible" "$post_unreachable" "$post_fail" \
    "$post_unsupported" "${icfg:-NA}" "${analyzed_icfg:-NA}" \
    "${functions:-NA}" "${analyzed_functions:-NA}" >> "$results"
}

total_attempts=$((warmups + measured_runs))
for ((attempt = 0; attempt < total_attempts; ++attempt)); do
  if ((attempt < warmups)); then
    phase=warmup
    run=$((attempt + 1))
  else
    phase=measured
    run=$((attempt - warmups + 1))
  fi
  for ((program_index = 0; program_index < ${#programs[@]}; ++program_index)); do
    relative=${programs[$program_index]}
    program=${relative%/*}
    input_id=$(sha256sum "$dataset_root/$relative" | cut -d' ' -f1)
    rotation=$(((program_index + attempt) % ${#configs[@]}))
    for ((position = 0; position < ${#configs[@]}; ++position)); do
      config=${configs[$(((position + rotation) % ${#configs[@]}))]}
      sparsity=${config%%:*}
      domain=${config#*:}
      run_one "$relative" "$program" "$input_id" "$phase" "$run" \
        "$sparsity" "$domain"
    done
  done
done

expected=$((total_attempts * ${#programs[@]} * ${#configs[@]}))
actual=$(($(wc -l < "$results") - 1))
if ((actual != expected)); then
  echo "benchmark matrix incomplete: expected $expected rows, got $actual" >&2
  exit 1
fi

echo "$results"
