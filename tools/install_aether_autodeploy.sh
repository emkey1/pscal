#!/usr/bin/env bash
# Install the post-commit hook that auto-deploys the Aether compiler to every benchmark
# host whenever a commit touches components/{aether,rea,pscal-core}.
# Run once per clone.
set -eu
ROOT="$(git rev-parse --show-toplevel)"
SRC="$ROOT/tools/git-hooks/post-commit"
DST="$ROOT/.git/hooks/post-commit"

# Refuse to clobber an UNRELATED hook, but happily upgrade our own (any earlier
# version calls a tools/deploy_aether_to_* script).
if [ -e "$DST" ] \
   && ! grep -q "deploy_aether_to_" "$DST" 2>/dev/null; then
    echo "WARNING: an existing, unrelated post-commit hook is present at $DST"
    echo "Move/merge it manually, then re-run. (Refusing to overwrite it.)"
    exit 1
fi
cp "$SRC" "$DST"
chmod +x "$DST"
echo "Installed aether auto-deploy hook -> $DST"
echo "Commits touching components/{aether,rea,pscal-core} now rebuild every bench host's"
echo "aether in the background (parallel, best-effort; hosts: AETHER_DEPLOY_HOSTS)."
echo "Log: /tmp/aether_autodeploy.log   Disable per-commit with: AETHER_AUTODEPLOY=0 git commit ..."
