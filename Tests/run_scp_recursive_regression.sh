#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SMALLCLUE="${ROOT}/build/bin/smallclue"

if [ ! -x "${SMALLCLUE}" ]; then
  echo "Missing ${SMALLCLUE}. Build with: cmake --build build --target smallclue" >&2
  exit 1
fi

echo "Running scp recursive absolute-path regression test..."

TEST_ROOT="$(mktemp -d "${ROOT}/build/.tmp-scp-recursive.XXXXXX")"
cleanup() {
  rm -rf "${TEST_ROOT}"
}
trap cleanup EXIT

mkdir -p "${TEST_ROOT}/src/sub"
printf "hello from scp recursion test\n" > "${TEST_ROOT}/src/sub/file.txt"

STDOUT_LOG="${TEST_ROOT}/stdout.log"
STDERR_LOG="${TEST_ROOT}/stderr.log"
if ! "${SMALLCLUE}" scp -r "${TEST_ROOT}/src" "${TEST_ROOT}/dst" \
    >"${STDOUT_LOG}" 2>"${STDERR_LOG}"; then
  echo "FAIL: scp -r absolute path copy failed" >&2
  cat "${STDERR_LOG}" >&2
  exit 1
fi

if [ ! -f "${TEST_ROOT}/dst/sub/file.txt" ]; then
  echo "FAIL: missing copied file after scp -r absolute path copy" >&2
  echo "stdout:" >&2
  cat "${STDOUT_LOG}" >&2
  echo "stderr:" >&2
  cat "${STDERR_LOG}" >&2
  exit 1
fi

if ! cmp -s "${TEST_ROOT}/src/sub/file.txt" "${TEST_ROOT}/dst/sub/file.txt"; then
  echo "FAIL: copied file content mismatch" >&2
  exit 1
fi

echo "scp recursive absolute-path regression: passed"
