#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SMALLCLUE="${ROOT}/build/bin/smallclue"

if [[ ! -x "${SMALLCLUE}" ]]; then
  echo "Missing ${SMALLCLUE}. Build with: cmake --build build --target smallclue" >&2
  exit 1
fi

echo "Running smallclue rsync regression test..."

TEST_ROOT="$(mktemp -d "${ROOT}/build/.tmp-rsync-regression.XXXXXX")"
trap 'rm -rf "${TEST_ROOT}"' EXIT

mkdir -p "${TEST_ROOT}/src/sub" "${TEST_ROOT}/dst"
printf "alpha\n" > "${TEST_ROOT}/src/sub/file.txt"
printf "root\n" > "${TEST_ROOT}/src/root.txt"
chmod 600 "${TEST_ROOT}/src/root.txt"
printf "stale\n" > "${TEST_ROOT}/dst/stale.txt"

"${SMALLCLUE}" rsync -av --delete "${TEST_ROOT}/src/" "${TEST_ROOT}/dst/" >"${TEST_ROOT}/run1.out" 2>&1

if [[ ! -f "${TEST_ROOT}/dst/sub/file.txt" ]]; then
  echo "FAIL: missing synced file ${TEST_ROOT}/dst/sub/file.txt" >&2
  cat "${TEST_ROOT}/run1.out" >&2 || true
  exit 1
fi

if [[ -e "${TEST_ROOT}/dst/stale.txt" ]]; then
  echo "FAIL: --delete did not remove stale destination file" >&2
  cat "${TEST_ROOT}/run1.out" >&2 || true
  exit 1
fi

dst_mode="$(stat -f %Lp "${TEST_ROOT}/dst/root.txt" 2>/dev/null || stat -c %a "${TEST_ROOT}/dst/root.txt")"
if [[ "${dst_mode}" != "600" ]]; then
  echo "FAIL: mode preservation mismatch (expected 600, got ${dst_mode})" >&2
  cat "${TEST_ROOT}/run1.out" >&2 || true
  exit 1
fi

printf "new\n" > "${TEST_ROOT}/src/new.txt"
"${SMALLCLUE}" rsync -avn --delete "${TEST_ROOT}/src/" "${TEST_ROOT}/dst/" >"${TEST_ROOT}/run2.out" 2>&1

if [[ -e "${TEST_ROOT}/dst/new.txt" ]]; then
  echo "FAIL: dry-run unexpectedly created ${TEST_ROOT}/dst/new.txt" >&2
  cat "${TEST_ROOT}/run2.out" >&2 || true
  exit 1
fi

if ! grep -q "copy " "${TEST_ROOT}/run2.out"; then
  echo "FAIL: dry-run output did not report planned copy operations" >&2
  cat "${TEST_ROOT}/run2.out" >&2 || true
  exit 1
fi

printf "source-older\n" > "${TEST_ROOT}/src/update.txt"
sleep 1
printf "destination-newer\n" > "${TEST_ROOT}/dst/update.txt"
"${SMALLCLUE}" rsync -au "${TEST_ROOT}/src/update.txt" "${TEST_ROOT}/dst/update.txt" >"${TEST_ROOT}/run3.out" 2>&1
if [[ "$(cat "${TEST_ROOT}/dst/update.txt")" != "destination-newer" ]]; then
  echo "FAIL: -u should have kept newer destination file" >&2
  cat "${TEST_ROOT}/run3.out" >&2 || true
  exit 1
fi

printf "AAAA\n" > "${TEST_ROOT}/src/checksum.txt"
printf "BBBB\n" > "${TEST_ROOT}/dst/checksum.txt"
touch -r "${TEST_ROOT}/src/checksum.txt" "${TEST_ROOT}/dst/checksum.txt"
"${SMALLCLUE}" rsync -a "${TEST_ROOT}/src/checksum.txt" "${TEST_ROOT}/dst/checksum.txt" >"${TEST_ROOT}/run4.out" 2>&1
if [[ "$(cat "${TEST_ROOT}/dst/checksum.txt")" != "BBBB" ]]; then
  echo "FAIL: baseline non-checksum sync should have skipped same size+mtime file" >&2
  cat "${TEST_ROOT}/run4.out" >&2 || true
  exit 1
fi
"${SMALLCLUE}" rsync -ac "${TEST_ROOT}/src/checksum.txt" "${TEST_ROOT}/dst/checksum.txt" >"${TEST_ROOT}/run5.out" 2>&1
if [[ "$(cat "${TEST_ROOT}/dst/checksum.txt")" != "AAAA" ]]; then
  echo "FAIL: -c should have detected content mismatch and copied file" >&2
  cat "${TEST_ROOT}/run5.out" >&2 || true
  exit 1
fi

mkdir -p "${TEST_ROOT}/src/filter/nested" "${TEST_ROOT}/dst/filter"
printf "keep-one\n" > "${TEST_ROOT}/src/filter/keep.txt"
printf "drop-one\n" > "${TEST_ROOT}/src/filter/drop.log"
printf "keep-two\n" > "${TEST_ROOT}/src/filter/nested/keep2.txt"
printf "drop-two\n" > "${TEST_ROOT}/src/filter/nested/drop2.log"
"${SMALLCLUE}" rsync -av --include="*.txt" "${TEST_ROOT}/src/filter/" "${TEST_ROOT}/dst/filter/" >"${TEST_ROOT}/run6.out" 2>&1
if [[ ! -f "${TEST_ROOT}/dst/filter/keep.txt" || ! -f "${TEST_ROOT}/dst/filter/nested/keep2.txt" ]]; then
  echo "FAIL: --include did not preserve expected *.txt files" >&2
  cat "${TEST_ROOT}/run6.out" >&2 || true
  exit 1
fi
if [[ -e "${TEST_ROOT}/dst/filter/drop.log" || -e "${TEST_ROOT}/dst/filter/nested/drop2.log" ]]; then
  echo "FAIL: --include should have omitted non-matching files" >&2
  cat "${TEST_ROOT}/run6.out" >&2 || true
  exit 1
fi

REMOTE_HOST="example-remote"
"${SMALLCLUE}" rsync -avn "${TEST_ROOT}/src/" "${REMOTE_HOST}:/tmp/rsync-dst/" >"${TEST_ROOT}/run7.out" 2>&1
if ! grep -q "copy ${TEST_ROOT}/src/ -> ${REMOTE_HOST}:/tmp/rsync-dst/" "${TEST_ROOT}/run7.out"; then
  echo "FAIL: remote upload dry-run did not report planned copy" >&2
  cat "${TEST_ROOT}/run7.out" >&2 || true
  exit 1
fi

"${SMALLCLUE}" rsync -avn "${REMOTE_HOST}:/tmp/rsync-src/" "${TEST_ROOT}/dst/" >"${TEST_ROOT}/run8.out" 2>&1
if ! grep -q "copy ${REMOTE_HOST}:/tmp/rsync-src/ -> ${TEST_ROOT}/dst/" "${TEST_ROOT}/run8.out"; then
  echo "FAIL: remote download dry-run did not report planned copy" >&2
  cat "${TEST_ROOT}/run8.out" >&2 || true
  exit 1
fi

if "${SMALLCLUE}" rsync -au "${REMOTE_HOST}:/tmp/rsync-src/file.txt" "${TEST_ROOT}/dst/file.txt" >"${TEST_ROOT}/run9.out" 2>&1; then
  echo "FAIL: remote -u should currently be rejected" >&2
  cat "${TEST_ROOT}/run9.out" >&2 || true
  exit 1
fi
if ! grep -q -- "-u/-c/--include/--exclude are not supported for remote transfers yet" "${TEST_ROOT}/run9.out"; then
  echo "FAIL: remote unsupported-option error message missing" >&2
  cat "${TEST_ROOT}/run9.out" >&2 || true
  exit 1
fi

echo "smallclue rsync regression: passed"
