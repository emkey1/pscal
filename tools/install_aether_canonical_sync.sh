#!/usr/bin/env bash
# Installs a post-commit hook in the canonical emkey1/aether standalone clone
# that pushes the commit and syncs PBuild's components/aether submodule
# checkout to match -- keeping "PSCAL Aether" and "the Aether repo" identical
# (per user directive, 2026-07-19). Run once per machine; idempotent.
#
# Usage: tools/install_aether_canonical_sync.sh [path-to-canonical-clone]
#        (defaults to ~/git/aether)
set -eu
CANONICAL_REPO="${1:-$HOME/git/aether}"
PBUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SYNC_SCRIPT="$PBUILD_ROOT/tools/sync_aether_canonical_repo.sh"
SRC="$PBUILD_ROOT/tools/git-hooks/aether-canonical-post-commit"

if [ ! -d "$CANONICAL_REPO/.git" ]; then
    echo "install_aether_canonical_sync: $CANONICAL_REPO is not a git repo" >&2
    exit 1
fi

DST="$CANONICAL_REPO/.git/hooks/post-commit"
if [ -e "$DST" ] && ! grep -q "AETHER_CANONICAL_SYNC" "$DST" 2>/dev/null; then
    echo "WARNING: an existing, unrelated post-commit hook is present at $DST"
    echo "Move/merge it manually, then re-run. (Refusing to overwrite it.)"
    exit 1
fi

chmod +x "$SYNC_SCRIPT"
sed "s#__PBUILD_SYNC_SCRIPT__#$SYNC_SCRIPT#" "$SRC" > "$DST"
chmod +x "$DST"
echo "Installed canonical-repo sync hook -> $DST"
echo "Commits in $CANONICAL_REPO now auto-push and sync $PBUILD_ROOT/components/aether"
echo "(and its own gitlink + the existing all-claws deploy hook, transitively) in the background."
echo "Sync log: /tmp/aether_canonical_sync.log   Disable per-commit with: AETHER_CANONICAL_SYNC=0 git commit ..."
