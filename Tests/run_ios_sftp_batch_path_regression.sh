#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXSH="${ROOT}/build/ios-host/bin/exsh"
HOST="no-such-host.invalid"

if [ ! -x "${EXSH}" ]; then
  echo "Missing ${EXSH}. Build with: cmake --build build/ios-host --target exsh" >&2
  exit 1
fi

echo "Running iOS sftp batch absolute-path regression test..."

TEST_SYSROOT="$(mktemp -d "${ROOT}/build/.tmp-sftp-batch-path.XXXXXX")"
BATCH_FILE="$(mktemp "${ROOT}/build/.tmp-sftp-batch-path.cmd.XXXXXX")"
cleanup() {
  rm -rf "${TEST_SYSROOT}" "${BATCH_FILE}"
}
trap cleanup EXIT

mkdir -p "${TEST_SYSROOT}/etc"
cat > "${TEST_SYSROOT}/etc/passwd" <<EOF
tester:x:$(id -u):$(id -g):Tester:/tmp:/bin/sh
EOF
cat > "${TEST_SYSROOT}/etc/group" <<EOF
tester:x:$(id -g):
EOF
cat > "${BATCH_FILE}" <<EOF
quit
EOF

OUTPUT="$("${EXSH}" -c "export PSCALI_ETC_ROOT=${TEST_SYSROOT}/etc; sftp -b ${BATCH_FILE} ${HOST}" 2>&1 || true)"

if [[ "${OUTPUT}" != *"Could not resolve hostname ${HOST}"* ]]; then
  echo "FAIL: expected unresolved-host error for ${HOST}" >&2
  echo "${OUTPUT}" >&2
  exit 1
fi

if [[ "${OUTPUT}" == *"No such file or directory"* ]]; then
  echo "FAIL: sftp batch path rewrite/regression detected" >&2
  echo "${OUTPUT}" >&2
  exit 1
fi

echo "sftp batch absolute-path regression: passed"
