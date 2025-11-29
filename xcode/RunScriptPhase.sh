#!/usr/bin/env bash
set -euo pipefail

# PSCAL iOS: De-duplicate shared core before linking both front-ends.
# This script is intended to be called from an Xcode Run Script build phase as:
#   bash "${PROJECT_DIR}/RunScriptPhase.sh"
# It invokes the repository tool at ../ios/Tools/dedupe_pscal_libs.sh.

# Which library should keep the shared core? 'rea' (default) or 'pascal'.
KEEP_CORE_IN="${PSCALI_KEEP_CORE_IN:-${KEEP_CORE_IN:-rea}}"

# Auto-detect simulator vs device from SDK_NAME.
if [[ "${SDK_NAME:-}" == iphonesimulator* ]]; then
  platform_dir="ios-simulator"
else
  platform_dir="ios-device"
fi

export KEEP_CORE_IN
export PSCAL_LIB_DIR="${PSCALI_LIB_DIR:-${PROJECT_DIR}/../build/${platform_dir}}"

echo "[pscal-run-script] KEEP_CORE_IN=${KEEP_CORE_IN}"
echo "[pscal-run-script] PSCAL_LIB_DIR=${PSCAL_LIB_DIR}"

echo "[pscal-run-script] Running de-duplication tool..."
bash "${PROJECT_DIR}/../ios/Tools/dedupe_pscal_libs.sh"
echo "[pscal-run-script] Done."
