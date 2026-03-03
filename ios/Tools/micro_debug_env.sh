#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage:
  micro_debug_env.sh --exports
  micro_debug_env.sh [command [args...]]

Examples:
  eval "$(ios/Tools/micro_debug_env.sh --exports)"
  ios/Tools/micro_debug_env.sh xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -destination 'generic/platform=iOS' build

If no command is provided, this script prints the debug variables and exits.
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if [[ "${1:-}" == "--exports" ]]; then
    cat <<'EOF'
export PSCALI_MICRO_RESIZE_TRACE=1
export PSCALI_MICRO_DEBUG=1
export PSCALI_IO_DEBUG=1
export PSCALI_SSH_RESIZE_DEBUG=1
export PSCALI_PTY_TRACE=1
EOF
    exit 0
fi

export PSCALI_MICRO_RESIZE_TRACE=1
export PSCALI_MICRO_DEBUG=1
export PSCALI_IO_DEBUG=1
export PSCALI_SSH_RESIZE_DEBUG=1
export PSCALI_PTY_TRACE=1

if [[ "$#" -eq 0 ]]; then
    env | grep -E '^PSCALI_(MICRO_RESIZE_TRACE|MICRO_DEBUG|IO_DEBUG|SSH_RESIZE_DEBUG|PTY_TRACE)=' | sort
    exit 0
fi

exec "$@"
