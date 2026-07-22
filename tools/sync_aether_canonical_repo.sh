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

# This script is launched via `nohup ... &` from the canonical repo's
# post-commit hook, which inherits its environment straight from git --
# including GIT_DIR/GIT_WORK_TREE/GIT_INDEX_FILE/GIT_PREFIX/
# GIT_OBJECT_DIRECTORY, which git sets (relative to the canonical repo) for
# the hook's own use. Left set, they leak into every `git -C ...` call below
# and get resolved against the wrong repo -- e.g. GIT_INDEX_FILE=.git/index
# resolves against the submodule checkout, whose .git is a plaintext gitlink
# file rather than a directory, so git fails with
# ".git/index: index file open failed: Not a directory" on every attempt
# (confirmed 2026-07-22). Unset them so every git call below resolves its
# repo purely from -C, as if run fresh in an interactive shell.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_PREFIX GIT_OBJECT_DIRECTORY

PBUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUBMODULE_DIR="$PBUILD_ROOT/components/aether"
LOG=/tmp/aether_canonical_sync.log

log() { echo "[sync-aether $(date '+%H:%M:%S')] $*" | tee -a "$LOG" >&2; }

# This script is launched backgrounded from a git hook right as the triggering
# commit finishes, so a concurrent `git status`/`log`/etc. in the same
# checkout (e.g. a human or agent immediately inspecting the result) can
# transiently collide on .git's index/lock files. Retry with exponential
# backoff before treating any of these steps as a real failure -- observed
# collisions have outlasted a flat 4x2s (~11s) window twice, so this ramps
# 2/4/8/16/32/32s (max 7 attempts, ~110s worst case) instead.
retry_git() {
    local attempt=1 max=7 delay=2 max_delay=32
    while [ "$attempt" -le "$max" ]; do
        if "$@" >>"$LOG" 2>&1; then
            return 0
        fi
        if [ "$attempt" -lt "$max" ]; then
            log "transient failure on '$*' (attempt $attempt/$max), retrying in ${delay}s"
            sleep "$delay"
            delay=$((delay * 2))
            [ "$delay" -gt "$max_delay" ] && delay="$max_delay"
        fi
        attempt=$((attempt + 1))
    done
    return 1
}

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
if ! retry_git git -C "$SUBMODULE_DIR" fetch origin; then
    log "fetch failed after retries, skipping sync"
    exit 0
fi
git -C "$SUBMODULE_DIR" checkout main >/dev/null 2>&1
if ! retry_git git -C "$SUBMODULE_DIR" merge --ff-only origin/main; then
    log "WARNING: fast-forward failed after retries (local main diverged from origin?) -- resolve manually in $SUBMODULE_DIR"
    exit 0
fi
AFTER="$(git -C "$SUBMODULE_DIR" rev-parse HEAD 2>/dev/null)"

# Deliberately NOT short-circuiting on BEFORE == AFTER here: that used to
# exit immediately with "already in sync", but BEFORE/AFTER only reflect
# whether *this invocation's* fetch+merge moved the submodule -- if an
# earlier run already fast-forwarded the submodule but died (e.g. the
# git-index race below) before committing the gitlink bump in PBuild, the
# submodule can be fully caught up while PBuild's own tracked pointer is
# still stale and uncommitted. Falling through to the actual
# `git status --porcelain components/aether` check below covers both cases
# correctly: nothing to commit when truly in sync, a pending bump otherwise.
cd "$PBUILD_ROOT" || exit 0
if [ -n "$(git status --porcelain components/aether)" ]; then
    SUBJECT="$(git -C "$SUBMODULE_DIR" log -1 --format='%s' "$AFTER")"
    if git add components/aether \
        && git commit -m "chore: bump aether gitlink -- $SUBJECT" >>"$LOG" 2>&1 \
        && git push >>"$LOG" 2>&1; then
        log "synced components/aether ${BEFORE:0:7} -> ${AFTER:0:7} and pushed gitlink bump"
    else
        log "WARNING: submodule fast-forwarded to ${AFTER:0:7} but the PBuild gitlink commit/push failed -- check $LOG and finish manually"
    fi
else
    log "already in sync at $AFTER"
fi
