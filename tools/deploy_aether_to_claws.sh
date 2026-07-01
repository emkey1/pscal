#!/usr/bin/env bash
# Deploy the current Aether compiler to ALL benchmark claws (claw1, claw2, claw3) so
# every eval box tests the just-committed language. Each host clones emkey1/aether at
# the EXACT commit recorded in PBuild's components/aether gitlink and rebuilds
# $HOME/aether-current/build/aether via tools/refresh_aether.sh (shipped to the host).
#
# Design notes:
#  * Builds the gitlink SHA, not "latest main", so the deployed binary provably matches
#    the code this PBuild commit points at (and an unpushed SHA fails loudly per host).
#  * Best-effort + PARALLEL: every host deploys in its own background job with a remote
#    build timeout; an unreachable/slow/broken claw is logged and skipped, and never
#    blocks the commit or the other hosts. Invoked (already backgrounded) by the git
#    post-commit hook; also safe to run by hand:  bash tools/deploy_aether_to_claws.sh
#  * Replaces the old claw2-only deploy_aether_to_claw2.sh, whose remote full-pscal
#    configure failed because it never shipped the Pascal sources that configure needs.
#
# Env overrides:
#   AETHER_CLAWS          space-separated "name|user@primary|user@fallback" specs
#   AETHER_DEPLOY_PUSH=0  skip the upstream-push safety step
#   AETHER_DEPLOY_TIMEOUT per-host remote build cap in seconds (default 1800)
#   AETHER_TARGET_SHA     override the aether commit to build (default: gitlink SHA)
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AETHER_DIR="$ROOT/components/aether"
REFRESH_SRC="$ROOT/tools/refresh_aether.sh"
DEPLOY_TIMEOUT="${AETHER_DEPLOY_TIMEOUT:-1800}"

# name|primary|fallback  (fallback optional; claw3 keeps its LAN address as a backup
# because MagicDNS occasionally lags). Override wholesale via $AETHER_CLAWS.
if [ -n "${AETHER_CLAWS:-}" ]; then
  # shellcheck disable=SC2206
  HOSTS=( $AETHER_CLAWS )
else
  HOSTS=(
    "claw1|claw@claw1.tailfe3968.ts.net|"
    "claw2|claw@claw2.tailfe3968.ts.net|"
    "claw3|claw@claw3.tailfe3968.ts.net|claw@192.168.110.27"
  )
fi

SSH="ssh -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=4"
SCP="scp -o BatchMode=yes -o ConnectTimeout=15"

log() { echo "[deploy $(date -u +%H:%M:%S)] $*"; }

# The aether commit to build = the SHA recorded in this commit's components/aether
# gitlink (falls back to the checkout's HEAD). Overridable for manual runs.
SHA="${AETHER_TARGET_SHA:-}"
[ -z "$SHA" ] && SHA="$(git -C "$ROOT" ls-tree HEAD components/aether 2>/dev/null | awk '{print $3}')"
[ -z "$SHA" ] && SHA="$(git -C "$AETHER_DIR" rev-parse HEAD 2>/dev/null)"
VER="$(cat "$AETHER_DIR/VERSION" 2>/dev/null || echo '?')"

if [ -z "$SHA" ]; then
  log "FATAL: could not determine aether commit to deploy (no gitlink, no checkout)"; exit 1
fi

# Safety net: make sure the target commit is actually on origin before hosts try to
# clone it. Normal workflow pushes aether before bumping the gitlink, so this is usually
# a no-op ("Everything up-to-date"). GitHub rejects non-fast-forward pushes, so this can
# never clobber origin/main. Non-fatal: if it fails we still try to build (the SHA may
# already be present as an ancestor).
if [ "${AETHER_DEPLOY_PUSH:-1}" != "0" ]; then
  if git -C "$AETHER_DIR" push origin "${SHA}:refs/heads/main" >/dev/null 2>&1; then
    log "ensured ${SHA} is on origin/main"
  else
    log "WARN: push of ${SHA} to origin/main did not fast-forward (continuing; hosts need it present)"
  fi
fi

# Deploy one host: pick a reachable address, ship refresh_aether.sh, build the target
# SHA under a remote timeout, then verify the binary runs and reports VERSION $VER.
deploy_host() {
  local spec="$1"
  local name primary fallback host rc got smoke hlog
  name="${spec%%|*}"
  local rest="${spec#*|}"
  primary="${rest%%|*}"
  fallback="${rest#*|}"; [ "$fallback" = "$primary" ] && fallback=""
  hlog="/tmp/aether_deploy_${name}.log"

  # Choose primary if reachable, else fallback, else give up (best-effort).
  host="$primary"
  if ! $SSH "$primary" true >/dev/null 2>&1; then
    if [ -n "$fallback" ] && $SSH "$fallback" true >/dev/null 2>&1; then
      host="$fallback"; log "[$name] primary unreachable -> using fallback ${fallback}"
    else
      log "[$name] UNREACHABLE -- skipped (host down or SSH blocked)"; return 1
    fi
  fi

  # Ship the canonical build script so every host builds identically (keeps claw1/claw3
  # in sync with claw2). Best-effort: fall back to the host's existing copy if scp fails.
  if $SCP "$REFRESH_SRC" "$host:refresh_aether.sh" >/dev/null 2>&1; then
    $SSH "$host" 'chmod +x ~/refresh_aether.sh' >/dev/null 2>&1
  else
    log "[$name] WARN: could not copy refresh_aether.sh (using host's existing copy)"
  fi

  log "[$name] building aether ${SHA} (VERSION ${VER}) on ${host} ... [detail: ${hlog}]"
  $SSH "$host" "timeout ${DEPLOY_TIMEOUT} bash ~/refresh_aether.sh ${SHA}" >"$hlog" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then
    log "[$name] BUILD FAILED (rc=${rc}$( [ $rc -eq 124 ] && echo ', timed out' )) -- see ${hlog}"
    return 1
  fi

  # Verify: binary runs AND reports the VERSION of the code we just built.
  got="$($SSH "$host" 'timeout 30 ~/aether-current/build/aether --version 2>&1 | head -1')"
  if ! printf '%s' "$got" | grep -q -- "$VER"; then
    log "[$name] VERSION MISMATCH -- expected ${VER}, got: ${got:-<none>}"
    return 1
  fi

  # Smoke: compile + run 21+21 through the binary (best-effort; a syntax drift in the
  # smoke program should not fail an otherwise-current deploy).
  smoke="$($SSH "$host" 'printf "fn main() -> Void {\n    let x: Int = 21;\n    fx { println(x + x); }\n    ret;\n}\n" > /tmp/deploy_smoke.aether && timeout 30 ~/aether-current/build/aether --no-cache /tmp/deploy_smoke.aether 2>&1' | tail -1)"
  if [ "$smoke" = "42" ]; then
    log "[$name] OK -- ${got} (smoke 42)"
  else
    log "[$name] OK -- ${got} (WARN smoke got: ${smoke})"
  fi
  return 0
}

log "=== deploy aether ${SHA} (VERSION ${VER}) -> ${#HOSTS[@]} claws (parallel, ${DEPLOY_TIMEOUT}s/host) ==="
pids=""
names=""
for spec in "${HOSTS[@]}"; do
  deploy_host "$spec" &
  pids="$pids $!"
  names="$names ${spec%%|*}"
done

ok=0; fail=0
set -- $names
for p in $pids; do
  n="$1"; shift
  if wait "$p"; then ok=$((ok+1)); else fail=$((fail+1)); fi
done
log "=== done: ${ok}/${#HOSTS[@]} hosts current, ${fail} failed/skipped ==="
# Exit non-zero only if EVERY host failed (useful for manual runs); the hook ignores it.
[ "$ok" -gt 0 ]
