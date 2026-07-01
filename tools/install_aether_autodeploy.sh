#!/usr/bin/env bash
# Install the post-commit hook that auto-deploys the Aether compiler to all benchmark
# claws (claw1, claw2, claw3) whenever a commit touches components/{aether,rea,pscal-core}.
# Run once per clone.
set -eu
ROOT="$(git rev-parse --show-toplevel)"
SRC="$ROOT/tools/git-hooks/post-commit"
DST="$ROOT/.git/hooks/post-commit"

# Refuse to clobber an UNRELATED hook, but happily upgrade our own (either the current
# all-claws marker or the retired claw2-only one).
if [ -e "$DST" ] \
   && ! grep -q "deploy_aether_to_claws" "$DST" 2>/dev/null \
   && ! grep -q "deploy_aether_to_claw2" "$DST" 2>/dev/null; then
    echo "WARNING: an existing, unrelated post-commit hook is present at $DST"
    echo "Move/merge it manually, then re-run. (Refusing to overwrite it.)"
    exit 1
fi
cp "$SRC" "$DST"
chmod +x "$DST"
echo "Installed aether all-claws auto-deploy hook -> $DST"
echo "Commits touching components/{aether,rea,pscal-core} now rebuild claw1/claw2/claw3's"
echo "aether in the background (parallel, best-effort)."
echo "Log: /tmp/aether_autodeploy.log   Disable per-commit with: AETHER_AUTODEPLOY=0 git commit ..."
