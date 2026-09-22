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
max_vars=${SVF_AE_RELATIONAL_MAX_VARS:-32}
fixture_dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$output_dir"

witnesses=(
  RelationalWitness RelationalCallWitness RelationalCallerFrameWitness
)
configs=(
  'box:whole:through'
  'octagon:whole:through'
  'octagon:whole:intraprocedural'
  'octagon:query-slice:through'
  'octagon:query-slice:intraprocedural'
  'octagon:eva-style:through'
  'octagon:query-eva:through'
  'octagon:query-eva:intraprocedural'
)
modes=(dense semi-sparse)
summary="$output_dir/summary.tsv"
printf 'witness\tmode\tdomain\tpolicy\tcalls\tstatus\tpost_pass\tpost_infeasible\tpost_unreachable\tpost_fail\tpost_unsupported\tquery_identity_hash\tquery_outcome_hash\tsafe\tmay\tunreachable\tunsupported\tseeds\trecognized_pairs\tcandidates\tselected\tdropped\n' > "$summary"

failed=0
for witness in "${witnesses[@]}"; do
  input="$fixture_dir/$witness.ll"
  input_id=$(sha256sum "$input" | awk '{print $1}')
  for mode in "${modes[@]}"; do
    for config in "${configs[@]}"; do
      IFS=: read -r domain policy calls <<< "$config"
      stem="$output_dir/$witness-$mode-$domain-$policy-$calls"
      timeout "$timeout_seconds" env SVF_AE_DOMAIN_STATS=1 "$ae_bin" \
        -extapi="$extapi_bc" \
        -ae-domain="$domain" \
        -ae-relational-policy="$policy" \
        -ae-relational-calls="$calls" \
        -ae-relational-max-vars="$max_vars" \
        -ae-sparsity="$mode" \
        -model-consts=true -model-arrays=true \
        -pre-field-sensitive=false -stat=false \
        -overflow=true -null-deref=true \
        -ae-query-input-id="$input_id" \
        -ae-query-ledger="$stem.queries.tsv" \
        -ae-post-check="$stem.post.tsv" \
        "$input" > "$stem.stdout" 2> "$stem.stderr"
      status=$?

      post_counts='0\t0\t0\t0\t0'
      if [[ -f $stem.post.tsv ]]; then
        post_counts=$(awk -F '\t' '
          NR > 1 { count[$6]++ }
          END { printf "%d\t%d\t%d\t%d\t%d", count["Pass"] + 0,
            count["Infeasible"] + 0, count["Unreachable"] + 0,
            count["Fail"] + 0, count["Unsupported"] + 0 }' "$stem.post.tsv")
      fi
      identity_hash=missing
      outcome_hash=missing
      query_counts='0\t0\t0\t0'
      if [[ -f $stem.queries.tsv ]]; then
        identity_hash=$(tail -n +2 "$stem.queries.tsv" | cut -f1 |
                        LC_ALL=C sort | sha256sum | awk '{print $1}')
        outcome_hash=$(tail -n +2 "$stem.queries.tsv" | cut -f1,9 |
                       LC_ALL=C sort | sha256sum | awk '{print $1}')
        query_counts=$(awk -F '\t' '
          NR > 1 { count[$9]++ }
          END { printf "%d\t%d\t%d\t%d", count["Safe"] + 0,
            count["May"] + 0, count["Unreachable"] + 0,
            count["Unsupported"] + 0 }' "$stem.queries.tsv")
      fi
      policy_line=$(grep 'AE_RELATIONAL_POLICY ' "$stem.stdout" | tail -n 1)
      field() {
        sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p" <<< "$policy_line"
      }
      printf '%s\t%s\t%s\t%s\t%s\t%d\t%b\t%s\t%s\t%b\t%s\t%s\t%s\t%s\t%s\n' \
        "$witness" "$mode" "$domain" "$policy" "$calls" "$status" \
        "$post_counts" "$identity_hash" "$outcome_hash" "$query_counts" \
        "$(field seeds || true)" "$(field recognized_pairs || true)" \
        "$(field candidates || true)" "$(field selected || true)" \
        "$(field dropped || true)" >> "$summary"
      if ((status != 0)); then
        failed=1
      fi
    done
  done
done

if ! awk -F '\t' 'NR > 1 && ($10 != 0 || $11 != 0 || $17 != 0) { exit 1 }' \
    "$summary"; then
  echo "Post/query unsupported gate failed" >&2
  failed=1
fi

for witness in "${witnesses[@]}"; do
  identities=$(awk -F '\t' -v w="$witness" '$1 == w { print $12 }' \
               "$summary" | LC_ALL=C sort -u | wc -l | tr -d ' ')
  if [[ $identities != 1 ]]; then
    echo "query identity coverage mismatch: $witness" >&2
    failed=1
  fi
  for config in "${configs[@]}"; do
    IFS=: read -r domain policy calls <<< "$config"
    dense=$(awk -F '\t' -v w="$witness" -v d="$domain" -v p="$policy" \
      -v c="$calls" '$1 == w && $2 == "dense" && $3 == d && $4 == p && $5 == c { print $13 }' "$summary")
    semi=$(awk -F '\t' -v w="$witness" -v d="$domain" -v p="$policy" \
      -v c="$calls" '$1 == w && $2 == "semi-sparse" && $3 == d && $4 == p && $5 == c { print $13 }' "$summary")
    if [[ -z $dense || $dense != "$semi" ]]; then
      echo "dense/semi outcome mismatch: $witness $domain $policy $calls" >&2
      failed=1
    fi
  done
done

# Semantic witnesses, not just count/coverage checks:
# - local affine relations eliminate the impossible branch;
# - through-call propagation eliminates the call witness while the Eva-default
#   intraprocedural boundary conservatively leaves it May;
# - caller-local relations survive either call policy.
if ! awk -F '\t' '
  NR == 1 { next }
  $3 == "octagon" && $1 == "RelationalWitness" && $16 != 1 { bad = 1 }
  $3 == "octagon" && $1 == "RelationalCallWitness" &&
      $5 == "through" && $16 != 1 { bad = 1 }
  $3 == "octagon" && $1 == "RelationalCallWitness" &&
      $5 == "intraprocedural" && $15 != 1 { bad = 1 }
  $3 == "octagon" && $1 == "RelationalCallerFrameWitness" && $16 != 1 {
    bad = 1
  }
  $3 == "octagon" && $4 != "whole" && ($21 == "" || $21 == 0) { bad = 1 }
  END { exit bad }
' "$summary"; then
  echo "Octagon strategy semantic witness gate failed" >&2
  failed=1
fi

echo "$summary"
exit "$failed"
