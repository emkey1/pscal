#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${ROOT}/build/bin/exsh"
TMP_ROOT="${HOME:-/tmp}/jobspec_tmp"

mkdir -p "${TMP_ROOT}"

if [[ ! -x "${BIN}" ]]; then
  echo "exsh binary not found at ${BIN}" >&2
  exit 77
fi

fail() { echo "FAIL: $*" >&2; exit 1; }

# Extract a jobs subsection between markers.
jobs_block() {
  local output="$1" start="$2" end="$3"
  printf "%s\n" "${output}" | sed -n "/${start}/,/${end}/p" | sed -e "1d" -e '$d'
}

case_kill_percent_one() {
  local body="
set -e
set -m
echo --J1--
sleep 60 &
sleep 60 &
jobs
echo --K1--
kill %1
sleep 1
echo --J2--
jobs
echo --K2--
kill %2 || true
sleep 1
echo --J3--
jobs
"
  local script="${TMP_ROOT}/jobspec_case1.exsh"
  printf "%s\n" "${body}" > "${script}"
  local output
  output=$("${BIN}" --norc --noprofile "${script}") || fail "case_kill_percent_one: exsh status $?"
  echo "${output}"
  local j1 j2
  j1=$(jobs_block "${output}" "--J1--" "--K1--")
  echo "${j1}" | grep -q "\\[1\\]" || fail "jobs list missing [1] in first snapshot"
  echo "${j1}" | grep -q "\\[2\\]" || fail "jobs list missing [2] in first snapshot"
  j2=$(jobs_block "${output}" "--J2--" "--K2--")
  echo "${j2}" | grep -q "\\[1\\]" && fail "jobs list still contains [1] after kill %1"
  echo "${j2}" | grep -q "\\[2\\]" || fail "jobs list missing surviving [2] after kill %1"
}

case_kill_middle_preserves_ids() {
  local body="
set -e
set -m
sleep 60 &
sleep 60 &
sleep 60 &
echo --J1--
jobs
echo --Kmid--
kill %2
sleep 1
echo --J2--
jobs
echo --Kall--
kill %1 || true
kill %3 || true
"
  local script="${TMP_ROOT}/jobspec_case2.exsh"
  printf "%s\n" "${body}" > "${script}"
  local output
  output=$("${BIN}" --norc --noprofile "${script}") || fail "case_kill_middle_preserves_ids: exsh status $?"
  echo "${output}"
  local j1 j2
  j1=$(jobs_block "${output}" "--J1--" "--Kmid--")
  echo "${j1}" | grep -q "\\[1\\]" || fail "first snapshot missing [1]"
  echo "${j1}" | grep -q "\\[2\\]" || fail "first snapshot missing [2]"
  echo "${j1}" | grep -q "\\[3\\]" || fail "first snapshot missing [3]"
  j2=$(jobs_block "${output}" "--J2--" "--Kall--")
  echo "${j2}" | grep -q "\\[1\\]" || fail "after kill %2 missing [1]"
  echo "${j2}" | grep -q "\\[3\\]" || fail "after kill %2 missing [3]"
  echo "${j2}" | grep -q "\\[2\\]" && fail "after kill %2 still contains [2]"
}

main() {
  if [[ ! -x "${BIN}" ]]; then
    echo "SKIP: exsh binary not found at ${BIN}"
    exit 77
  fi
  case_kill_percent_one
  case_kill_middle_preserves_ids
  echo "All jobspec tests passed."
}

main "$@"
