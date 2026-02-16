#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXSH="${ROOT}/build/ios-host/bin/exsh"

if [ ! -x "${EXSH}" ]; then
  echo "Missing ${EXSH}. Build with: cmake --build build/ios-host --target exsh" >&2
  exit 1
fi

echo "Running iOS ssh virtual-home regression test..."

TEST_ROOT="$(mktemp -d "${ROOT}/build/.tmp-ssh-virtual-home.XXXXXX")"
cleanup() {
  rm -rf "${TEST_ROOT}"
}
trap cleanup EXIT

mkdir -p "${TEST_ROOT}/home/.ssh"
mkdir -p "${TEST_ROOT}/etc"
# Ensure NSS lookups succeed inside the virtualized root.
cat > "${TEST_ROOT}/etc/passwd" <<EOF
tester:x:$(id -u):$(id -g):Tester:/home:/bin/sh
EOF
cat > "${TEST_ROOT}/etc/group" <<EOF
tester:x:$(id -g):
EOF
# Minimal private-key placeholder; ssh -G does not parse key contents but this
# keeps the fixture realistic and catches accidental path probing regressions.
cat > "${TEST_ROOT}/home/.ssh/id_ed25519" <<'KEY'
-----BEGIN OPENSSH PRIVATE KEY-----
not-a-real-key
-----END OPENSSH PRIVATE KEY-----
KEY
chmod 700 "${TEST_ROOT}/home/.ssh"
chmod 600 "${TEST_ROOT}/home/.ssh/id_ed25519"

CMD="export PATH_TRUNCATE=${TEST_ROOT}; export PSCALI_ETC_ROOT=${TEST_ROOT}/etc; export HOME=${TEST_ROOT}/home; ssh -G -F /dev/null pi"
OUTPUT="$(${EXSH} -c "${CMD}" 2>&1 || true)"

if [[ "${OUTPUT}" != *"userknownhostsfile /home/.ssh/known_hosts /home/.ssh/known_hosts2"* ]]; then
  echo "FAIL: expected known-hosts paths to resolve under /home/.ssh" >&2
  echo "${OUTPUT}" >&2
  exit 1
fi

if [[ "${OUTPUT}" == *"userknownhostsfile /.ssh/"* ]]; then
  echo "FAIL: regression detected: HOME collapsed to '/'." >&2
  echo "${OUTPUT}" >&2
  exit 1
fi

if [[ "${OUTPUT}" == *"${TEST_ROOT}"* ]]; then
  echo "FAIL: regression detected: physical test root leaked in ssh -G output" >&2
  echo "${OUTPUT}" >&2
  exit 1
fi

echo "ssh virtual-home regression: passed"
