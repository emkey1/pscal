#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  filter_micro_debug_logs.sh < xcode_console.log
  xcodebuild ... 2>&1 | ios/Tools/filter_micro_debug_logs.sh

This keeps only high-signal lines for current micro/iOS PTY debug:
  - micro resize/bridge lifecycle
  - session winsize updates
  - hterm/runtime resize forwarding and binding
  - bridge failures / embedded panic markers
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

regex='(\[micro-resize\] micro (bridgeSetup active|bridgeSetup failed|bridge requested|bridgeStateSet|launch seeded env|launch no-session-size|launch has no session id|notifySessionWinsize|bridgeApplySessionWinsize|applyBridgeWinsize|signalResize|ioBridgeThread exit))|(\[micro-resize\] runtime updateSessionWindowSize)|(\[micro-resize\] vproc setSessionWinsize (applied|missing-pty|invalid))|(\[ssh-resize\] runtime bind session)|(\[ssh-resize\] hterm\[[0-9]+\] (bind-session|runtime-forward|runtime-defer|native-resize|request-resize gen|request-resize result|force-grid req|force-grid applied))|(micro: (unable to initialize PTY bridge|PTY bridge setup failed|already running|internal panic|panic stack))|(PSCALRuntime: (launching exsh|launch aborted))|(\[runtime-io\] winsize)'

grep -E --line-buffered "$regex"
