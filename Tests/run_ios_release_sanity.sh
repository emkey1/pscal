#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTS_DIR="${ROOT}/Tests"
IOS_PROJECT="${ROOT}/ios/PSCAL.xcodeproj"
IOS_SCHEME="PscalApp"

run_step() {
  local label="$1"
  shift
  echo
  echo "==> ${label}"
  "$@"
}

echo "Running iOS/iPadOS release sanity suite..."

run_step "Vproc/TTY shim unit checks" \
  bash "${TESTS_DIR}/run_ios_port_tests.sh"
run_step "Extended builtin inventory parity (incl. SDL/3D entries)" \
  bash "${TESTS_DIR}/run_ios_ext_builtin_sanity.sh"
run_step "SCP prompt unit regression" \
  bash "${TESTS_DIR}/run_ios_scp_prompt_regression.sh"

# RUN_INTERACTIVE_SIGNAL_TESTS=1 can be exported by callers to include PTY
# Ctrl-C/Ctrl-Z checks during the host suite run.
run_step "iOS-host exsh regression subset" \
  bash "${TESTS_DIR}/run_exsh_ios_host_tests.sh"
run_step "SFTP unresolved-host regression" \
  bash "${TESTS_DIR}/run_ios_sftp_hostname_regression.sh"
run_step "SSH virtual-home regression" \
  bash "${TESTS_DIR}/run_ios_ssh_virtual_home_regression.sh"
run_step "SCP auth-prompt liveness regression" \
  bash "${TESTS_DIR}/run_ios_scp_authprompt_regression.sh"

if [ "${RUN_IOS_PRESET_BUILDS:-0}" = "1" ]; then
  run_step "Configure ios-simulator preset" cmake --preset ios-simulator
  run_step "Build ios-simulator preset" cmake --build --preset ios-simulator
  run_step "Configure ios-device preset" cmake --preset ios-device
  run_step "Build ios-device preset" cmake --build --preset ios-device
fi

if [ "${RUN_IOS_XCODEBUILD:-0}" = "1" ]; then
  run_step "Xcode generic iOS build (codesign disabled)" \
    xcodebuild -project "${IOS_PROJECT}" \
      -scheme "${IOS_SCHEME}" \
      -configuration Debug \
      -destination "generic/platform=iOS" \
      CODE_SIGNING_ALLOWED=NO \
      CODE_SIGNING_REQUIRED=NO \
      CODE_SIGN_IDENTITY="" \
      build
fi

echo
echo "iOS/iPadOS release sanity suite passed."
