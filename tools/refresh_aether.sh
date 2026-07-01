#!/usr/bin/env bash
# Rebuild the benchmark `aether` binary on THIS host from the CANONICAL split repo
# (emkey1/aether), which vendors rea + pscal-core as git submodules under external/.
# Produces $HOME/aether-current/build/aether -- the path the eval harness points at
# (run_eval_v2.sh --aether-bin /home/claw/aether-current/build/aether). Run before a
# sweep or after any language change so benchmarks always test the live language.
#
# This is the single source of truth for "how to build the bench aether on a host"; it
# is shipped verbatim to every claw by tools/deploy_aether_to_claws.sh, so all three
# eval boxes build the identical way. It replaces the old, drift-prone full-pscal /
# monorepo configure (which failed because it needed Pascal sources it never shipped).
#
# Usage: refresh_aether.sh [TARGET]
#   TARGET   git ref/SHA to build. Default: whatever the clone's default branch (main)
#            points at. The PBuild autodeploy passes the EXACT aether commit recorded in
#            its components/aether gitlink, so every claw builds the same just-committed
#            language -- and a not-yet-pushed SHA fails loudly here (see exit 2) instead
#            of silently building stale main.
#
# Builds into a fresh temp tree and swaps it into place only on success, so a failed
# refresh leaves the last-good binary untouched. Idempotent + safe to re-run.
#
# Env overrides: AETHER_REPO_URL, AETHER_DEST, AETHER_REFRESH_LOG.
set -u

TARGET="${1:-}"
REPO_URL="${AETHER_REPO_URL:-https://github.com/emkey1/aether.git}"
DEST="${AETHER_DEST:-$HOME/aether-current}"
LOG="${AETHER_REFRESH_LOG:-/tmp/aether_refresh.log}"
NEW="${DEST}.new.$$"

log() { echo "[refresh $(date -u +%H:%M:%S)] $*"; }
cleanup() { rm -rf "$NEW" 2>/dev/null; }
trap cleanup EXIT

: > "$LOG" 2>/dev/null || true

log "clone ${REPO_URL} (target: ${TARGET:-default branch}) -> ${NEW}"
# Full clone (NOT --depth 1) so any commit is checkoutable; submodules are pulled after
# checkout so they match the target commit's pinned SHAs, not main's.
if ! git clone -q "$REPO_URL" "$NEW" >>"$LOG" 2>&1; then
  log "FAILED: git clone (see $LOG)"; exit 1
fi
cd "$NEW" || { log "FAILED: cd $NEW"; exit 1; }

if [ -n "$TARGET" ]; then
  if ! git checkout -q "$TARGET" >>"$LOG" 2>&1; then
    log "FAILED: checkout ${TARGET} -- not on origin? push aether upstream first (see $LOG)"
    exit 2
  fi
fi
if ! git submodule update --init --recursive >>"$LOG" 2>&1; then
  log "FAILED: submodule update (rea/pscal-core) (see $LOG)"; exit 3
fi

BUILT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo '?')"
BUILT_VER="$(cat VERSION 2>/dev/null || echo '?')"
log "configure + build (sha ${BUILT_SHA}, VERSION ${BUILT_VER})"
if ! cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >>"$LOG" 2>&1; then
  log "FAILED: cmake configure (see $LOG)"; exit 4
fi
if ! cmake --build build -j"$(nproc 2>/dev/null || echo 8)" >>"$LOG" 2>&1; then
  log "FAILED: cmake build (see $LOG)"; exit 5
fi
if [ ! -x build/aether ]; then
  log "FAILED: build/aether missing after build (see $LOG)"; exit 6
fi

# Atomically swap the fresh tree into place, keeping the previous build as .old until the
# move succeeds (so an interrupted swap never leaves us with no binary).
cd "$HOME" || exit 7
rm -rf "${DEST}.old" 2>/dev/null
[ -e "$DEST" ] && mv "$DEST" "${DEST}.old" 2>/dev/null
if ! mv "$NEW" "$DEST"; then
  log "FAILED: could not swap ${NEW} -> ${DEST}"
  [ -e "${DEST}.old" ] && mv "${DEST}.old" "$DEST" 2>/dev/null
  exit 8
fi
rm -rf "${DEST}.old" 2>/dev/null
trap - EXIT

BIN="${DEST}/build/aether"
# Back-compat: keep the legacy harness path pointing at the fresh binary, if it exists.
if [ -d "$HOME/pscal-bench/build/bin" ]; then
  ln -sf "$BIN" "$HOME/pscal-bench/build/bin/aether" 2>/dev/null || true
fi

log "refreshed: $("$BIN" --version 2>/dev/null | head -1 || echo built)"
