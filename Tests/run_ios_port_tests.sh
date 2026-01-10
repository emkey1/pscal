#!/bin/bash
set -euo pipefail

# Dedicated entrypoint for iOS/iPadOS portability checks.  These exercises the
# vproc/tty shims separately from the default suites so desktop runs aren’t
# blocked by platform-specific probes.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

pushd "${SCRIPT_DIR}" >/dev/null
trap 'popd >/dev/null' EXIT

bash ./run_ios_vproc_tests.sh
