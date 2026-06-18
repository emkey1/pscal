#!/usr/bin/env bash
# Deploy the local Aether compiler to the claw2 eval box, so benchmark runs always
# evaluate model output against the CURRENT language (fixes + enhancements).
#
# Aether is its own repo now (components/aether), built standalone via its own
# CMake with FetchContent pointed at the LOCAL core/rea checkouts. So this syncs
# the aether/core/rea component sources to claw2's pscal-bench2 tree and rebuilds
# the standalone `aether` binary there. The eval harness's aether path
# (/home/claw/pscal-bench/build/bin/aether) is a symlink to that binary, so the
# whole harness picks up the rebuild transparently.
#
# Safe to re-run. Invoked automatically by the git post-commit hook (installed via
# tools/install_claw2_autodeploy.sh) whenever a commit touches the compiler
# sources. Override the target with CLAW2_HOST / CLAW2_PSCAL2 / CLAW2_EVAL_BIN.
set -u
CLAW="${CLAW2_HOST:-claw@100.124.15.16}"
REMOTE="${CLAW2_PSCAL2:-/home/claw/pscal-bench2}"
AETHER_DIR="${REMOTE}/components/aether"
AETHER_BUILD="${AETHER_DIR}/build"
EVAL_BIN="${CLAW2_EVAL_BIN:-/home/claw/pscal-bench/build/bin/aether}"
LOCAL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=20"

echo "[deploy $(date -u +%H:%M:%S)] sync aether/core/rea sources -> ${CLAW}:${REMOTE}"
# rsync -a preserves mtimes, which can make cmake skip a changed file (its mtime
# may be older than the existing .o). So capture the files rsync actually transfers
# (-i) and `touch` them on the remote, forcing a recompile of exactly those files.
# The standalone aether build pulls core + rea straight from these local trees.
sync_tree() {
  local rel="$1"
  local changed
  changed=$(rsync -az -i --exclude '*.o' -e "$SSH" "${LOCAL}/${rel}/" "${CLAW}:${REMOTE}/${rel}/" \
    | awk '/^[<>]f/{print $2}') || { echo "[deploy] rsync ${rel} FAILED"; exit 1; }
  if [ -n "$changed" ]; then
    local n
    n=$(printf '%s\n' "$changed" | wc -l | tr -d ' ')
    printf '%s\n' "$changed" | $SSH "$CLAW" "cd '${REMOTE}/${rel}' && xargs touch"
    echo "[deploy] re-stamped ${n} changed file(s) in ${rel}"
  else
    echo "[deploy] no changes in ${rel}"
  fi
}
sync_tree "components/aether/src"
sync_tree "components/pscal-core/src"
sync_tree "components/rea/src"

echo "[deploy] (re)configure if needed + build standalone aether on claw2 (incremental)"
$SSH "$CLAW" "[ -f '${AETHER_BUILD}/CMakeCache.txt' ] || cmake -S '${AETHER_DIR}' -B '${AETHER_BUILD}' \
  -DFETCHCONTENT_SOURCE_DIR_REA='${REMOTE}/components/rea' \
  -DFETCHCONTENT_SOURCE_DIR_PSCAL_CORE='${REMOTE}/components/pscal-core' \
  -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1" \
  || { echo "[deploy] configure FAILED"; exit 1; }
$SSH "$CLAW" "cmake --build '${AETHER_BUILD}' --target aether -j8 2>&1 | tail -3" \
  || { echo "[deploy] build FAILED"; exit 1; }

# Keep the eval harness's aether path pointing at the freshly built binary.
$SSH "$CLAW" "ln -sf '${AETHER_BUILD}/aether' '${EVAL_BIN}'"

echo "[deploy] smoke test (multi-line fx + arithmetic via eval path)"
SMOKE=$($SSH "$CLAW" "printf 'fn main() -> Void {\n    let x: Int = 21;\n    fx { println(x + x); }\n    ret;\n}\n' > /tmp/deploy_smoke.aether && '${EVAL_BIN}' --no-cache /tmp/deploy_smoke.aether 2>&1")
if [ "$SMOKE" = "42" ]; then
  echo "[deploy $(date -u +%H:%M:%S)] OK — claw2 aether is current"
else
  echo "[deploy] SMOKE FAILED (got: ${SMOKE})"; exit 1
fi
