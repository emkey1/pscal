#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXSH="${ROOT}/build/ios-host/bin/exsh"

if [ ! -x "${EXSH}" ]; then
  echo "Missing ${EXSH}. Build with: cmake --build build/ios-host --target exsh" >&2
  exit 1
fi

echo "Running iOS scp authprompt regression test..."

TEST_SYSROOT="$(mktemp -d "${ROOT}/build/.tmp-scp-authprompt.XXXXXX")"
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

export TEST_SYSROOT

expect <<'EOF'
set timeout 30
spawn /Users/mke/PBuild/build/ios-host/bin/exsh -c "export PSCALI_ETC_ROOT=$env(TEST_SYSROOT)/etc; export PSCAL_VPROC_TEST_CHILD_MODE=authprompt_hold; scp -S pscal-vproc-test-child tester@local:/tmp/none /tmp"

expect {
  -re {vproc-test-child password:} {}
  eof { puts "FAIL: scp exited before password prompt"; exit 1 }
  timeout { puts "FAIL: timed out waiting for password prompt"; exit 1 }
}

# Before any input, process must still be alive.
expect {
  eof { puts "FAIL: scp exited before first character"; exit 1 }
  timeout {}
} 1

send -- "s"

# After first character, process must still be alive (bug symptom check).
expect {
  eof { puts "FAIL: scp exited after first character"; exit 1 }
  timeout {}
} 1

send -- "ecret\r"

# Process must still be alive right after full password submit.
expect {
  eof { puts "FAIL: scp exited immediately after full password submit"; exit 1 }
  timeout {}
} 1

# Explicitly stop the hanging transfer stub.
send -- "\003"
expect eof
exit 0
EOF

echo "scp authprompt regression: passed"
