#!/usr/bin/env bash
# Deploy the local Aether compiler to the claw2 eval box: sync the compiler
# source and rebuild the `aether` binary there, so benchmark runs always evaluate
# model output against the CURRENT language (fixes + enhancements included).
#
# Safe to re-run. Invoked automatically by the git post-commit hook (installed via
# tools/install_claw2_autodeploy.sh) whenever a commit touches src/ or
# CMakeLists.txt. Override the target with CLAW2_HOST / CLAW2_PSCAL.
set -u
CLAW="${CLAW2_HOST:-claw@100.124.15.16}"
REMOTE="${CLAW2_PSCAL:-/home/claw/pscal-bench}"
LOCAL="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SSH="ssh -o BatchMode=yes -o ConnectTimeout=20"

echo "[deploy $(date -u +%H:%M:%S)] sync src + CMakeLists -> ${CLAW}:${REMOTE}"
# rsync -a preserves mtimes, which can make cmake skip a changed file (its mtime
# may be older than the existing .o). So capture the files rsync actually
# transfers (-i) and `touch` them on the remote, forcing a recompile of exactly
# those files (and nothing more).
CHANGED=$(rsync -az -i --exclude '*.o' -e "$SSH" "${LOCAL}/src/" "${CLAW}:${REMOTE}/src/" | awk '/^[<>]f/{print $2}')
rsync -az -e "$SSH" "${LOCAL}/CMakeLists.txt" "${CLAW}:${REMOTE}/CMakeLists.txt" \
  || { echo "[deploy] rsync FAILED"; exit 1; }
if [ -n "$CHANGED" ]; then
  N=$(printf '%s\n' "$CHANGED" | wc -l | tr -d ' ')
  printf '%s\n' "$CHANGED" | $SSH "$CLAW" "cd '${REMOTE}/src' && xargs touch"
  echo "[deploy] re-stamped ${N} changed source file(s) for rebuild"
else
  echo "[deploy] no source changes to sync"
fi

echo "[deploy] rebuild aether on claw2 (incremental)"
$SSH "$CLAW" "cd '${REMOTE}' && cmake --build build --target aether -j8 2>&1 | tail -3" \
  || { echo "[deploy] build FAILED"; exit 1; }

echo "[deploy] smoke test (multi-line fx + arithmetic)"
SMOKE=$($SSH "$CLAW" "printf 'fn main() -> Void {\n    let x: Int = 21;\n    fx { println(x + x); }\n    ret;\n}\n' > /tmp/deploy_smoke.aether && '${REMOTE}/build/bin/aether' --no-cache /tmp/deploy_smoke.aether 2>&1")
if [ "$SMOKE" = "42" ]; then
  echo "[deploy $(date -u +%H:%M:%S)] OK — claw2 aether is current"
else
  echo "[deploy] SMOKE FAILED (got: ${SMOKE})"; exit 1
fi
