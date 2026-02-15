#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXSH="${ROOT}/build/ios-host/bin/exsh"
HOST="no-such-host.invalid"

if [ ! -x "${EXSH}" ]; then
  echo "Missing ${EXSH}. Build with: cmake --build build/ios-host --target exsh" >&2
  exit 1
fi

echo "Running iOS sftp hostname regression test..."

TEST_SYSROOT="$(mktemp -d "${ROOT}/build/.tmp-sftp-hostname.XXXXXX")"
cleanup() {
  rm -rf "${TEST_SYSROOT}"
}
trap cleanup EXIT

mkdir -p "${TEST_SYSROOT}/etc"
cat > "${TEST_SYSROOT}/etc/passwd" <<EOF
tester:x:$(id -u):$(id -g):Tester:/tmp:/bin/sh
EOF
cat > "${TEST_SYSROOT}/etc/group" <<EOF
tester:x:$(id -g):
EOF

OUTPUT="$("${EXSH}" -c "export PSCALI_ETC_ROOT=${TEST_SYSROOT}/etc; sftp ${HOST}" 2>&1 || true)"

if [[ "${OUTPUT}" != *"Could not resolve hostname ${HOST}"* ]]; then
  echo "FAIL: expected unresolved-host error for ${HOST}" >&2
  echo "${OUTPUT}" >&2
  exit 1
fi

if [[ "${OUTPUT}" == *"Could not resolve hostname ssh"* ]]; then
  echo "FAIL: regression detected: sftp resolved literal hostname 'ssh'" >&2
  echo "${OUTPUT}" >&2
  exit 1
fi

echo "sftp hostname regression: passed"
