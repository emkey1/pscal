#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DVTM_DIR="${ROOT}/third-party/dvtm"

if [ ! -d "${DVTM_DIR}" ] || [ ! -f "${DVTM_DIR}/dvtm.c" ] || [ ! -f "${DVTM_DIR}/vt.c" ] || [ ! -f "${DVTM_DIR}/config.def.h" ]; then
  echo "error: dvtm source tree is missing required files under ${DVTM_DIR}" >&2
  exit 1
fi

DEVSDK="$(xcrun --sdk iphoneos --show-sdk-path)"
MACSDK="$(xcrun --sdk macosx --show-sdk-path)"
if [ ! -f "${MACSDK}/usr/include/curses.h" ]; then
  echo "error: missing curses headers in macOS SDK: ${MACSDK}/usr/include/curses.h" >&2
  exit 1
fi
HOOKS_HEADER="${ROOT}/src/smallclue/src/dvtm_runtime_hooks.h"
DVTM_APP_SRC="${ROOT}/src/smallclue/src/dvtm_app.c"
if [ ! -f "${HOOKS_HEADER}" ] || [ ! -f "${DVTM_APP_SRC}" ]; then
  echo "error: missing dvtm shim sources under ${ROOT}/src/smallclue/src" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ios-dvtm-sanity.XXXXXX")"
trap 'rm -rf "${TMP_DIR}"' EXIT

cp "${DVTM_DIR}/config.def.h" "${TMP_DIR}/config.h"
cat > "${TMP_DIR}/main.c" <<'EOF'
int main(void) { return 0; }
EOF

COMMON_FLAGS=(
  -arch arm64
  -isysroot "${DEVSDK}"
  -miphoneos-version-min=15.0
  -std=c99
  -Wno-nullability-completeness
  -D_POSIX_C_SOURCE=200809L
  -D_XOPEN_SOURCE=700
  -D_XOPEN_SOURCE_EXTENDED
  -D_DARWIN_C_SOURCE
  -DVERSION=\"0.16-pscal\"
  -Dexit=pscalDvtmRequestExit
  "-include${HOOKS_HEADER}"
  -I"${TMP_DIR}"
  -I"${DVTM_DIR}"
  -I"${ROOT}/src/smallclue/src"
  -I"${MACSDK}/usr/include"
)

echo "Compiling dvtm.c for iOS..."
clang "${COMMON_FLAGS[@]}" -Dmain=dvtm_main_entry -c "${DVTM_DIR}/dvtm.c" -o "${TMP_DIR}/dvtm.o"
echo "Compiling vt.c for iOS..."
clang "${COMMON_FLAGS[@]}" -c "${DVTM_DIR}/vt.c" -o "${TMP_DIR}/vt.o"
echo "Compiling dvtm_app shim for iOS..."
clang "${COMMON_FLAGS[@]}" -DSMALLCLUE_WITH_DVTM -c "${DVTM_APP_SRC}" -o "${TMP_DIR}/dvtm_app.o"
echo "Linking iOS dvtm sanity binary..."
clang -arch arm64 -isysroot "${DEVSDK}" -miphoneos-version-min=15.0 \
  "${TMP_DIR}/main.c" "${TMP_DIR}/dvtm.o" "${TMP_DIR}/vt.o" "${TMP_DIR}/dvtm_app.o" -lcurses -o "${TMP_DIR}/dvtm_sanity"

cat > "${TMP_DIR}/dvtm_exit_probe.c" <<'EOF'
#include <stdio.h>

int smallclueRunDvtm(int argc, char **argv);

int main(void) {
  char *argv[] = { "dvtm", "-v", NULL };
  int rc = smallclueRunDvtm(2, argv);
  printf("PSCAL_DVTM_PROBE_RETURNED rc=%d\n", rc);
  return (rc == 0) ? 0 : 2;
}
EOF

HOST_FLAGS=(
  -std=c99
  -D_POSIX_C_SOURCE=200809L
  -D_XOPEN_SOURCE=700
  -D_XOPEN_SOURCE_EXTENDED
  -D_DARWIN_C_SOURCE
  -DVERSION=\"0.16-pscal\"
  -Dexit=pscalDvtmRequestExit
  "-include${HOOKS_HEADER}"
  -DSMALLCLUE_WITH_DVTM
  -I"${TMP_DIR}"
  -I"${DVTM_DIR}"
  -I"${ROOT}/src/smallclue/src"
  -I"${MACSDK}/usr/include"
)

echo "Compiling host dvtm exit-shim regression probe..."
clang "${HOST_FLAGS[@]}" -Dmain=dvtm_main_entry -c "${DVTM_DIR}/dvtm.c" -o "${TMP_DIR}/dvtm_host.o"
clang "${HOST_FLAGS[@]}" -c "${DVTM_DIR}/vt.c" -o "${TMP_DIR}/vt_host.o"
clang "${HOST_FLAGS[@]}" -c "${DVTM_APP_SRC}" -o "${TMP_DIR}/dvtm_app_host.o"
clang "${HOST_FLAGS[@]}" -c "${TMP_DIR}/dvtm_exit_probe.c" -o "${TMP_DIR}/dvtm_exit_probe_main.o"
clang "${TMP_DIR}/dvtm_host.o" "${TMP_DIR}/vt_host.o" "${TMP_DIR}/dvtm_app_host.o" "${TMP_DIR}/dvtm_exit_probe_main.o" \
  -lcurses -lpthread -o "${TMP_DIR}/dvtm_exit_probe"

echo "Running host dvtm exit-shim regression probe..."
probe_output="$("${TMP_DIR}/dvtm_exit_probe" 2>&1 || true)"
echo "${probe_output}"
if ! printf '%s\n' "${probe_output}" | grep -q "PSCAL_DVTM_PROBE_RETURNED rc=0"; then
  echo "error: dvtm exit shim regression probe did not return through smallclueRunDvtm" >&2
  exit 1
fi

echo "Running host dvtm shell-selection regression probe..."
probe_shell_output="$(SHELL=/tmp/exsh PSCALI_DVTM_DEBUG=1 "${TMP_DIR}/dvtm_exit_probe" 2>&1 || true)"
echo "${probe_shell_output}"
if ! printf '%s\n' "${probe_shell_output}" | grep -q "\\[dvtm\\] launch"; then
  echo "error: dvtm shell-selection probe did not emit launch diagnostics" >&2
  exit 1
fi
if printf '%s\n' "${probe_shell_output}" | grep -q "shell=/tmp/exsh"; then
  echo "error: dvtm shell-selection probe kept problematic SHELL=/tmp/exsh" >&2
  exit 1
fi
if ! printf '%s\n' "${probe_shell_output}" | grep -q "PSCAL_DVTM_PROBE_RETURNED rc=0"; then
  echo "error: dvtm shell-selection probe did not return successfully" >&2
  exit 1
fi

echo "iOS dvtm sanity compile + exit-shim runtime regression passed."
