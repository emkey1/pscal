#!/usr/bin/env bash
# Bump BOTH of PBuild's independent pscal-core pins together in one step:
#   1. the top-level components/pscal-core submodule (used directly by
#      PBuild's own CMakeLists.txt / pscal_core_static, linked by several
#      frontend executables besides aether)
#   2. components/aether's nested external/pscal-core pin, reached by
#      checking out whichever aether commit already references the target
#      pscal-core SHA
# These two pins are genuinely independent (confirmed 2026-07-22) and NOTHING
# keeps them in sync automatically -- this script exists because they were
# found already diverged from ordinary work, not from anyone's mistake. See
# tools/check_pscal_core_sync.sh for the read-only drift check.
#
# Design notes:
#  * Nested pins are baked into aether's own git history: you can't retarget
#    components/aether/external/pscal-core without checking out a *different*
#    aether commit. If the pscal-core fix hasn't landed in
#    ~/git/aether/external/pscal-core (commit+push there) yet, no aether
#    commit will pin the new SHA and the aether side is left untouched -- fix
#    that upstream first, then re-run this script. See
#    tools/install_aether_canonical_sync.sh / sync_aether_canonical_repo.sh
#    for how a canonical-repo commit normally reaches PBuild's
#    components/aether gitlink.
#  * On full success (both pins matched and staged) it commits and pushes
#    automatically, mirroring sync_aether_canonical_repo.sh's philosophy for
#    gitlink bumps. If the aether side couldn't be matched, it stops after
#    staging the top-level pin and does NOT commit -- committing then would
#    just re-introduce drift.
#
# Usage: tools/bump_pscal_core.sh [TARGET_SHA]
#   TARGET_SHA   pscal-core commit/ref to bump to. Default: origin/main tip.
#
# Env overrides:
#   PSCAL_CORE_REMOTE_BRANCH  branch to resolve/search on (default: main)
#   PSCAL_CORE_BUMP_COMMIT=0  stage only, skip commit+push (old behavior)
#   PSCAL_CORE_BUMP_PUSH=0    commit locally but skip the push
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CORE_DIR="$ROOT/components/pscal-core"
AETHER_DIR="$ROOT/components/aether"
BRANCH="${PSCAL_CORE_REMOTE_BRANCH:-main}"

log() { echo "[bump-pscal-core $(date -u +%H:%M:%S)] $*"; }

if [ ! -d "$CORE_DIR/.git" ] && [ ! -f "$CORE_DIR/.git" ]; then
  log "FATAL: $CORE_DIR is not an initialized submodule (git submodule update --init components/pscal-core)"; exit 1
fi
if [ ! -d "$AETHER_DIR/.git" ] && [ ! -f "$AETHER_DIR/.git" ]; then
  log "FATAL: $AETHER_DIR is not an initialized submodule (git submodule update --init components/aether)"; exit 1
fi

log "fetching origin for pscal-core and aether..."
git -C "$CORE_DIR" fetch --quiet origin || { log "FATAL: fetch failed in $CORE_DIR"; exit 1; }
git -C "$AETHER_DIR" fetch --quiet origin || { log "FATAL: fetch failed in $AETHER_DIR"; exit 1; }

TARGET="${1:-}"
if [ -z "$TARGET" ]; then
  TARGET="$(git -C "$CORE_DIR" rev-parse "origin/${BRANCH}")"
  log "no TARGET_SHA given, using origin/${BRANCH} tip: $TARGET"
else
  TARGET="$(git -C "$CORE_DIR" rev-parse "${TARGET}^{commit}" 2>/dev/null)" \
    || { log "FATAL: '$1' does not resolve to a commit in $CORE_DIR"; exit 1; }
  log "target pscal-core commit: $TARGET"
fi

CURRENT_TOP="$(git -C "$ROOT" ls-tree HEAD -- components/pscal-core | awk '{print $3}')"
if [ "$CURRENT_TOP" = "$TARGET" ]; then
  log "top-level components/pscal-core already at $TARGET, nothing to do there"
else
  log "bumping top-level components/pscal-core: ${CURRENT_TOP:-?} -> $TARGET"
  git -C "$CORE_DIR" checkout --quiet "$TARGET" \
    || { log "FATAL: checkout of $TARGET failed in $CORE_DIR"; exit 1; }
fi

# Nested pin: find the newest aether commit on origin/<branch> whose
# external/pscal-core gitlink already equals TARGET. Fast path: the currently
# checked-out aether commit already matches (the common case -- the aether
# side is usually bumped first via its own canonical-repo sync hook, and only
# the top-level pin gets missed).
CURRENT_AETHER="$(git -C "$AETHER_DIR" rev-parse HEAD)"
NESTED_SHA="$(git -C "$AETHER_DIR" ls-tree HEAD -- external/pscal-core | awk '{print $3}')"

AETHER_OK=1
if [ "$NESTED_SHA" = "$TARGET" ]; then
  log "components/aether (already at ${CURRENT_AETHER:0:7}) already pins pscal-core @ $TARGET, nothing to do there"
else
  log "components/aether's current pscal-core pin ($NESTED_SHA) doesn't match target; searching origin/${BRANCH} history..."
  MATCH=""
  for c in $(git -C "$AETHER_DIR" log --format='%H' "origin/${BRANCH}" -- external/pscal-core); do
    s="$(git -C "$AETHER_DIR" ls-tree "$c" -- external/pscal-core | awk '{print $3}')"
    if [ "$s" = "$TARGET" ]; then MATCH="$c"; break; fi
  done

  if [ -n "$MATCH" ]; then
    log "found matching aether commit ${MATCH:0:7}, checking out"
    git -C "$AETHER_DIR" checkout --quiet "$MATCH" \
      || { log "FATAL: checkout of $MATCH failed in $AETHER_DIR"; exit 1; }
  else
    AETHER_OK=0
    log "WARNING: no commit on ${AETHER_DIR#$ROOT/}'s origin/${BRANCH} pins pscal-core @ $TARGET"
    log "         land the bump in ~/git/aether/external/pscal-core (commit+push there) first,"
    log "         let it reach origin/${BRANCH} (see tools/sync_aether_canonical_repo.sh), then re-run this script."
    log "         Leaving components/aether untouched (still at ${CURRENT_AETHER:0:7})."
  fi
fi

cd "$ROOT" || exit 1
git add components/pscal-core
[ "$AETHER_OK" = 1 ] && git add components/aether

echo
log "staged changes:"
git diff --cached --stat -- components/pscal-core components/aether | sed 's/^/  /'

if [ "$AETHER_OK" != 1 ]; then
  echo
  log "top-level pin staged, but components/aether could NOT be matched (see warning above)."
  log "Not committing -- committing now would leave the two pins diverged again."
  exit 2
fi

if [ -z "$(git diff --cached --name-only -- components/pscal-core components/aether)" ]; then
  echo
  log "both pins already in sync at pscal-core @ ${TARGET:0:7}, nothing to commit."
  exit 0
fi

if [ "${PSCAL_CORE_BUMP_COMMIT:-1}" = "0" ]; then
  echo
  log "both pins staged and in sync at pscal-core @ ${TARGET:0:7}. PSCAL_CORE_BUMP_COMMIT=0 set, skipping commit. Review, then:"
  echo "  git commit -m 'chore: bump pscal-core pins to ${TARGET:0:7}'"
  exit 0
fi

MSG="chore: bump pscal-core pins to ${TARGET:0:7}"
if ! git commit -m "$MSG" >/dev/null; then
  log "FATAL: commit failed -- pins remain staged, fix manually"; exit 1
fi
log "committed: $MSG"

if [ "${PSCAL_CORE_BUMP_PUSH:-1}" = "0" ]; then
  log "PSCAL_CORE_BUMP_PUSH=0 set, skipping push. Push manually when ready."
  exit 0
fi

if git push; then
  log "pushed."
else
  log "WARNING: commit succeeded locally but push failed -- push manually (see output above)."
  exit 1
fi
