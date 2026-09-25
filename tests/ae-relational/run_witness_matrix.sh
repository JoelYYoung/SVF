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

timeout_prefix=()
if command -v timeout >/dev/null 2>&1; then
  timeout_prefix=(timeout "$timeout_seconds")
elif command -v gtimeout >/dev/null 2>&1; then
  timeout_prefix=(gtimeout "$timeout_seconds")
fi
runtime_prefix=()
if [[ -n ${SVF_AE_DYLD_LIBRARY_PATH:-} ]]; then
  runtime_prefix=(env "DYLD_LIBRARY_PATH=$SVF_AE_DYLD_LIBRARY_PATH")
fi

witnesses=(
  RelationalWitness
  RelationalWrapWitness
  PolyhedraWitness
  RelationalPhiWitness
  RelationalPhiPartialWitness
  RelationalPhiTupleWitness
  RelationalExpressionBoundWitness
  RelationalQueryReconstructionWitness
  RelationalSelectWitness
  RelationalCallWitness
  RelationalCallWrapWitness
  RelationalIndirectReturnWitness
  RelationalLoopWitness
  RelationalMemoryWitness
  RelationalMemoryKillWitness
  BranchLocalWitness
  SharedCalleeWitness
  MultiAliasKillWitness
  RecursiveCallWitness
  MachineInteger8Witness
  ExternalReturnWitness
  PointerLoadWitness
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
      ${timeout_prefix[@]+"${timeout_prefix[@]}"} \
        ${runtime_prefix[@]+"${runtime_prefix[@]}"} "$ae_bin" \
        -extapi="$extapi_bc" \
        -ae-domain="$domain" \
        -ae-sparsity="$mode" \
      ${recursion[@]+"${recursion[@]}"} \
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
        # Coverage equality compares stable query identities only.  Outcomes
        # may legitimately differ across dense and semi-sparse precision.
        query_hash=$(tail -n +2 "$stem.queries.tsv" | cut -f1 |
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

# The assertion is concretely false on the zero-iteration path (%a == -3).
# No domain/mode may certify it as Safe; doing so indicates that a partial phi
# summary discarded a feasible predecessor.
for mode in "${modes[@]}"; do
  for domain in "${domains[@]}"; do
    ledger="$output_dir/RelationalPhiPartialWitness-$mode-$domain.queries.tsv"
    if ! awk -F '\t' '
      NR > 1 { total++; outcome[$9]++ }
      END { exit !(total == 1 && outcome["Safe"] == 0) }
    ' "$ledger"; then
      echo "partial phi soundness regression: $mode $domain" >&2
      failed=1
    fi
  done
done

# The second subtraction is bounded by the saved x-a=2 relation. Box has no
# such relation; Octagon and Polyhedra must prove it in both execution modes.
for mode in "${modes[@]}"; do
  for domain in "${domains[@]}"; do
    ledger="$output_dir/RelationalExpressionBoundWitness-$mode-$domain.queries.tsv"
    expected=May
    if [[ $domain == octagon || $domain == polyhedra ]]; then
      expected=Safe
    fi
    if ! awk -F '\t' -v expected="$expected" '
      NR > 1 && $3 == "assertion" { total++; outcome[$9]++ }
      END { exit !(total == 1 && outcome[expected] == 1) }
    ' "$ledger"; then
      echo "expression-bound outcome failed: $mode $domain expected=$expected" >&2
      failed=1
    fi
  done
done

# The guard and assertion use different subtraction temporaries. Box cannot
# connect them, while Octagon must prove the reconstructed a-b <= 1 predicate
# in both execution modes.
for mode in "${modes[@]}"; do
  for domain in "${domains[@]}"; do
    ledger="$output_dir/RelationalQueryReconstructionWitness-$mode-$domain.queries.tsv"
    expected=May
    if [[ $domain == octagon || $domain == polyhedra ]]; then
      expected=Safe
    fi
    if ! awk -F '\t' -v expected="$expected" '
      NR > 1 && $3 == "assertion" { total++; outcome[$9]++ }
      END { exit !(total == 1 && outcome[expected] == 1) }
    ' "$ledger"; then
      echo "query reconstruction outcome failed: $mode $domain expected=$expected" >&2
      failed=1
    fi
  done
done

# Split ICFG nodes must retain LLVM's simultaneous phi-tuple semantics. Box
# cannot prove the cross-target equality. Octagon and Polyhedra prove the
# positive assertion, while the paired false assertion remains May.
for mode in "${modes[@]}"; do
  for domain in "${domains[@]}"; do
    ledger="$output_dir/RelationalPhiTupleWitness-$mode-$domain.queries.tsv"
    expected_safe=0
    if [[ $domain == octagon || $domain == polyhedra ]]; then
      expected_safe=1
    fi
    if ! awk -F '\t' -v expected_safe="$expected_safe" '
      NR > 1 && $3 == "assertion" { total++; outcome[$9]++ }
      END {
        exit !(total == 2 && outcome["Safe"] == expected_safe &&
               outcome["May"] == 2 - expected_safe &&
               outcome["Unsupported"] == 0)
      }' "$ledger"; then
      echo "phi-tuple outcome failed: $mode $domain" >&2
      failed=1
    fi
  done
done

# A numerical relation update after a pointer store/load must not erase the
# address facet.  All domains therefore have the same result on this witness.
for mode in "${modes[@]}"; do
  outcomes=$(awk -F '\t' -v m="$mode" \
    '$1 == "PointerLoadWitness" && $2 == m { print $10 }' "$summary" |
    LC_ALL=C sort -u | wc -l | tr -d ' ')
  if [[ $outcomes != 1 ]]; then
    echo "pointer load outcome mismatch: $mode" >&2
    failed=1
  fi
  for domain in "${domains[@]}"; do
    ledger="$output_dir/PointerLoadWitness-$mode-$domain.queries.tsv"
    if ! awk -F '\t' '
      NR > 1 { count[$9]++; total++ }
      END {
        exit !(total == 2 && count["Safe"] == 1 &&
               count["Unreachable"] == 1 && count["May"] == 0 &&
               count["Unsupported"] == 0)
      }' "$ledger"; then
      echo "pointer load expected outcome failed: $mode $domain" >&2
      failed=1
    fi
  done
done

echo "$summary"
exit "$failed"
