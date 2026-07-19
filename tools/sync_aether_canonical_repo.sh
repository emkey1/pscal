#!/usr/bin/env bash
# Syncs the components/aether submodule checkout to match the canonical
# emkey1/aether standalone repo's current pushed state, then commits + pushes
# the resulting gitlink bump here in PBuild -- so "PSCAL Aether" and "the
# Aether repo" never silently drift (per user directive, 2026-07-19).
#
# Called automatically by the canonical repo's post-commit hook (see
# tools/install_aether_canonical_sync.sh) right after a push there; safe to
# re-run manually / on a schedule too -- it no-ops if already in sync.
#
# Best-effort, non-blocking, matches deploy_aether_to_claws.sh's philosophy:
# never force/discard anything, just skip + log loudly on any conflict for a
# human to resolve. Disable with AETHER_CANONICAL_SYNC=0.
set -u
[ "${AETHER_CANONICAL_SYNC:-1}" = "0" ] && exit 0

PBUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUBMODULE_DIR="$PBUILD_ROOT/components/aether"
LOG=/tmp/aether_canonical_sync.log

log() { echo "[sync-aether $(date '+%H:%M:%S')] $*" | tee -a "$LOG" >&2; }

if ! git -C "$SUBMODULE_DIR" rev-parse --git-dir >/dev/null 2>&1; then
    log "$SUBMODULE_DIR is not a git checkout, skipping"
    exit 0
fi

if [ -n "$(git -C "$SUBMODULE_DIR" status --porcelain)" ]; then
    log "WARNING: uncommitted changes in $SUBMODULE_DIR -- skipping sync." \
        "Commit/stash them there first, then re-run this script manually."
    exit 0
fi

BEFORE="$(git -C "$SUBMODULE_DIR" rev-parse HEAD 2>/dev/null)"
if ! git -C "$SUBMODULE_DIR" fetch origin >>"$LOG" 2>&1; then
    log "fetch failed, skipping sync"
    exit 0
fi
git -C "$SUBMODULE_DIR" checkout main >/dev/null 2>&1
if ! git -C "$SUBMODULE_DIR" merge --ff-only origin/main >>"$LOG" 2>&1; then
    log "WARNING: fast-forward failed (local main diverged from origin) -- resolve manually in $SUBMODULE_DIR"
    exit 0
fi
AFTER="$(git -C "$SUBMODULE_DIR" rev-parse HEAD 2>/dev/null)"

if [ "$BEFORE" = "$AFTER" ]; then
    log "already in sync at $AFTER"
    exit 0
fi

SUBJECT="$(git -C "$SUBMODULE_DIR" log -1 --format='%s' "$AFTER")"
cd "$PBUILD_ROOT" || exit 0
if [ -n "$(git status --porcelain components/aether)" ]; then
    if git add components/aether \
        && git commit -m "chore: bump aether gitlink -- $SUBJECT" >>"$LOG" 2>&1 \
        && git push >>"$LOG" 2>&1; then
        log "synced components/aether ${BEFORE:0:7} -> ${AFTER:0:7} and pushed gitlink bump"
    else
        log "WARNING: submodule fast-forwarded to ${AFTER:0:7} but the PBuild gitlink commit/push failed -- check $LOG and finish manually"
    fi
else
    log "submodule moved to ${AFTER:0:7} but PBuild's gitlink already matched (unexpected, no-op)"
fi
