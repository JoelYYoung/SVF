#!/usr/bin/env bash
set -uo pipefail

if (($# != 3)); then
  echo "usage: $0 AE_BIN EXTAPI_BC OUTPUT_DIR" >&2
  exit 2
fi

ae_bin=$1
extapi_bc=$2
output_dir=$3
timeout_seconds=${SVF_AE_WITNESS_TIMEOUT_SECONDS:-60}
fixture_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$output_dir"

witnesses=(
  RelationalWitness
  RelationalWrapWitness
  PolyhedraWitness
  RelationalPhiWitness
  RelationalSelectWitness
  RelationalCallWitness
  RelationalCallWrapWitness
  RelationalLoopWitness
  RelationalMemoryWitness
  RelationalMemoryKillWitness
  BranchLocalWitness
  SharedCalleeWitness
  MultiAliasKillWitness
  RecursiveCallWitness
  MachineInteger8Witness
  ExternalReturnWitness
)
modes=(dense semi-sparse)
domains=(box octagon polyhedra)
summary="$output_dir/summary.tsv"
printf 'witness\tmode\tdomain\tstatus\tpass\tinfeasible\tunreachable\tfail\tunsupported\tquery_hash\n' > "$summary"

failed=0
for witness in "${witnesses[@]}"; do
  input="$fixture_dir/$witness.ll"
  input_id=$(sha256sum "$input" | awk '{print $1}')
  recursion=()
  if [[ $witness == RecursiveCallWitness ]]; then
    recursion=(-handle-recur=top)
  fi
  for mode in "${modes[@]}"; do
    for domain in "${domains[@]}"; do
      stem="$output_dir/$witness-$mode-$domain"
      timeout "$timeout_seconds" "$ae_bin" \
        -extapi="$extapi_bc" \
        -ae-domain="$domain" \
        -ae-sparsity="$mode" \
        "${recursion[@]}" \
        -model-consts=true \
        -model-arrays=true \
        -pre-field-sensitive=false \
        -stat=false \
        -overflow=true \
        -null-deref=true \
        -ae-query-input-id="$input_id" \
        -ae-query-ledger="$stem.queries.tsv" \
        -ae-post-check="$stem.post.tsv" \
        "$input" > "$stem.stdout" 2> "$stem.stderr"
      status=$?

      counts='0\t0\t0\t0\t0'
      if [[ -f $stem.post.tsv ]]; then
        counts=$(awk -F '\t' '
          NR > 1 { count[$6]++ }
          END {
            printf "%d\t%d\t%d\t%d\t%d", count["Pass"] + 0,
              count["Infeasible"] + 0, count["Unreachable"] + 0,
              count["Fail"] + 0, count["Unsupported"] + 0
          }' "$stem.post.tsv")
      fi
      query_hash=missing
      if [[ -f $stem.queries.tsv ]]; then
        query_hash=$(tail -n +2 "$stem.queries.tsv" | cut -f1,9 |
                     LC_ALL=C sort | sha256sum | awk '{print $1}')
      fi
      printf '%s\t%s\t%s\t%d\t%b\t%s\n' "$witness" "$mode" "$domain" \
        "$status" "$counts" "$query_hash" >> "$summary"
      if ((status != 0)); then
        failed=1
      fi
    done
  done
done

if awk -F '\t' 'NR > 1 && ($8 != 0 || $9 != 0) { exit 1 }' "$summary"; then
  :
else
  failed=1
fi

for witness in "${witnesses[@]}"; do
  for domain in "${domains[@]}"; do
    dense=$(awk -F '\t' -v w="$witness" -v d="$domain" \
      '$1 == w && $2 == "dense" && $3 == d { print $10 }' "$summary")
    semi=$(awk -F '\t' -v w="$witness" -v d="$domain" \
      '$1 == w && $2 == "semi-sparse" && $3 == d { print $10 }' "$summary")
    if [[ -z $dense || $dense != "$semi" ]]; then
      echo "query mismatch: $witness $domain dense=$dense semi=$semi" >&2
      failed=1
    fi
  done
done

echo "$summary"
exit "$failed"
