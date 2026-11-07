#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/build"
OUT="${BUILD_DIR}/ios_vproc_test"
OUT_PGID="${BUILD_DIR}/ios_vproc_pgid_test"
OUT_TERMINAL="${BUILD_DIR}/ios_vproc_session_terminal_test"
OUT_SHIMS="${BUILD_DIR}/ios_vproc_signal_shim_test"
OUT_JOBCTRL="${BUILD_DIR}/ios_vproc_jobcontrol_shim_test"
OUT_RESIZE="${BUILD_DIR}/ios_vproc_resize_harness_test"
OUT_HOST_GUARDS="${BUILD_DIR}/ios_vproc_host_guards_test"
OUT_USER_DB="${BUILD_DIR}/ios_vproc_user_db_test"
OUT_FIRST_THREAD="${BUILD_DIR}/ios_vproc_first_thread_test"
OUT_UTMP_WRITERS="${BUILD_DIR}/ios_vproc_utmp_writers_test"
OUT_SELF_SIGNAL="${BUILD_DIR}/ios_vproc_self_signal_test"
OUT_NEXTVI_PASTE="${BUILD_DIR}/ios_nextvi_bracketed_paste_test"
OUT_PING_PACKET="${BUILD_DIR}/ios_ping_packet_test"
NEXTVI_OBJ="${BUILD_DIR}/nextvi_ios_regression_vi.o"
# The vproc/tty platform layer lives in pscal-core since the components split;
# only the back-compat headers and the session stub stayed in the umbrella's src/ios.
CORE="${ROOT}/components/pscal-core"
INCLUDE_DIRS=(
  "-I${ROOT}/src"
  "-I${ROOT}"
  "-I${CORE}/src"
  "-I${CORE}"
)

mkdir -p "${BUILD_DIR}"

VPROC_SOURCES=(
  "${CORE}/src/runtime/vproc/vproc.c"
  "${CORE}/src/common/frontend_kind.c"
  "${CORE}/src/common/path_virtualization.c"
  "${CORE}/src/common/env_snapshot.c"
  "${CORE}/src/common/path_truncate.c"
  "${CORE}/src/common/runtime_tty.c"
  "${ROOT}/src/ios/runtime_session_stub.c"
  "${CORE}/src/runtime/vproc/tty/pscal_compat.c"
  "${CORE}/src/runtime/vproc/tty/pscal_fd.c"
  "${CORE}/src/runtime/vproc/tty/pscal_tty.c"
  "${CORE}/src/runtime/vproc/tty/pscal_pty.c"
  "${CORE}/src/runtime/vproc/tty/pscal_tty_host.c"
  "${ROOT}/Tests/ios_vproc/test_smallclue_applets_stub.c"
)

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_vproc.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_pgid_sid.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_PGID}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_session_terminal.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_TERMINAL}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_signal_shims.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_SHIMS}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_jobcontrol_shims.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_JOBCTRL}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_resize_harness.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_RESIZE}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_host_guards.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_HOST_GUARDS}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_user_db.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_USER_DB}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_first_thread.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_FIRST_THREAD}"

cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_self_signal.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_SELF_SIGNAL}"

cc -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "-I${ROOT}/src/smallclue/src" \
   "${ROOT}/Tests/ios_vproc/test_utmp_writers.c" \
   "${ROOT}/src/smallclue/src/utmp_rec.c" \
   -o "${OUT_UTMP_WRITERS}"

cc -DPSCAL_TARGET_IOS=1 \
   -DNEXTVI_REGRESSION_TEST_HOOKS=1 \
   -Dmain=nextvi_main_entry \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   -c "${CORE}/third-party/nextvi/vi.c" \
   -o "${NEXTVI_OBJ}"

cc -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_nextvi_bracketed_paste_regression.c" \
   "${NEXTVI_OBJ}" \
   -o "${OUT_NEXTVI_PASTE}"
"${OUT_PING_PACKET}"

# The app's ping packets, from ios/Sources/Bridge; on macOS it pings 127.0.0.1 too.
cc -I"${ROOT}" \
   "${ROOT}/Tests/ios_vproc/test_ping_packet.c" \
   -o "${OUT_PING_PACKET}"

# test_host_guards signals groups and everything, trusting its own kill to catch a call that gets
# through. A name looked up or a raw syscall would pass that by and send a real one, so stop here.
if grep -rnE '"kill(pg)?"|SYS_kill' "${CORE}/src/runtime/vproc"; then
  echo "FAIL vproc reaches the host's kill in a way test_host_guards cannot catch" >&2
  exit 1
fi

# The terminal layer on its own, with fake process hooks and no vproc.
OUT_TTY_CONTRACT="${BUILD_DIR}/tty_contract_test"
cc -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/test_tty_contract.c" \
   "${CORE}/src/runtime/vproc/tty/pscal_compat.c" \
   "${CORE}/src/runtime/vproc/tty/pscal_fd.c" \
   "${CORE}/src/runtime/vproc/tty/pscal_tty.c" \
   "${CORE}/src/runtime/vproc/tty/pscal_pty.c" \
   "${CORE}/src/runtime/vproc/tty/pscal_tty_host.c" \
   -o "${OUT_TTY_CONTRACT}"

# What a program sees on a terminal, held to what it sees on Linux. See tty_diff.c for how
# the reference is made. A hang here would be the terminal's, so it has a limit.
OUT_TTY_DIFF="${BUILD_DIR}/ios_vproc_tty_diff"
cc -DVPROC_ENABLE_STUBS_FOR_TESTS=1 -DPSCAL_TARGET_IOS=1 \
   -pthread \
   "${INCLUDE_DIRS[@]}" \
   "${ROOT}/Tests/ios_vproc/tty_diff.c" \
   "${VPROC_SOURCES[@]}" \
   -o "${OUT_TTY_DIFF}"

echo "Running ios_vproc tests..."
"${OUT_TTY_CONTRACT}"
/usr/bin/perl -e 'alarm shift; exec @ARGV' 300 "${OUT_TTY_DIFF}" > "${OUT_TTY_DIFF}.out"
diff -u "${ROOT}/Tests/ios_vproc/tty_diff.linux.out" "${OUT_TTY_DIFF}.out"
/usr/bin/perl -e 'alarm shift; exec @ARGV' 60 "${OUT_TTY_DIFF}" root > "${OUT_TTY_DIFF}.root.out"
diff -u "${ROOT}/Tests/ios_vproc/tty_diff.root.linux.out" "${OUT_TTY_DIFF}.root.out"
"${OUT}"
"${OUT_PGID}"
"${OUT_TERMINAL}"
"${OUT_SHIMS}"
"${OUT_JOBCTRL}"
"${OUT_RESIZE}"
"${OUT_HOST_GUARDS}"
"${OUT_USER_DB}"
"${OUT_FIRST_THREAD}"
"${OUT_UTMP_WRITERS}"
"${OUT_SELF_SIGNAL}"
"${OUT_NEXTVI_PASTE}"
"${OUT_PING_PACKET}"
