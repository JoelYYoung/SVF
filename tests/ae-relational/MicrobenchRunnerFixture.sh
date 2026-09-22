#!/usr/bin/env bash
set -euo pipefail

if [[ ${1:-} == --header ]]; then
  printf 'fixture_header\n'
  exit 0
fi

printf 'fixture_header\nfixture_row\n'
case ${FIXTURE_BEHAVIOR:-success} in
success)
  ;;
fail)
  printf 'fixture failure\n' >&2
  exit 7
  ;;
timeout)
  printf 'fixture timeout\n' >&2
  sleep 5
  ;;
*)
  printf 'unknown fixture behavior\n' >&2
  exit 9
  ;;
esac
