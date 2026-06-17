#!/usr/bin/env bash
# Install the post-commit hook that auto-deploys the Aether compiler to claw2
# whenever a commit touches src/ or CMakeLists.txt. Run once per clone.
set -eu
ROOT="$(git rev-parse --show-toplevel)"
SRC="$ROOT/tools/git-hooks/post-commit"
DST="$ROOT/.git/hooks/post-commit"

if [ -e "$DST" ] && ! grep -q "deploy_aether_to_claw2" "$DST" 2>/dev/null; then
    echo "WARNING: an existing post-commit hook is present at $DST"
    echo "Move/merge it manually, then re-run. (Refusing to overwrite an unrelated hook.)"
    exit 1
fi
cp "$SRC" "$DST"
chmod +x "$DST"
echo "Installed claw2 auto-deploy hook -> $DST"
echo "Commits touching src/ or CMakeLists.txt now rebuild claw2's aether in the background."
echo "Log: /tmp/claw2_autodeploy.log   Disable per-commit with CLAW2_AUTODEPLOY=0 git commit ..."
