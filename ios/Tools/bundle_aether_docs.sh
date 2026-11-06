#!/usr/bin/env bash
#
# Stage the Aether guide into the app bundle's Docs/Aether, which
# RuntimeAssetInstaller copies to ~/Docs with the rest of Docs. Run after the
# Docs copy in "Bundle Toolchain Data", whose rsync --delete would remove it.
#
# The guide is the full one, aether_for_llms_and_others.md: it ends in the
# generated inventory of every builtin, which aether's standalone build
# produces. The categories that build leaves out (SDL graphics, 3D) are ones
# the inventory skips anyway, so it is the app's inventory too; the app smoke
# row aether_guide_inventory regenerates it from the app's builtins_json.
#
# Usage: bundle_aether_docs.sh <bundle_root>
set -euo pipefail

bundle_root="${1:?usage: bundle_aether_docs.sh <bundle_root>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
aether_docs="${repo_root}/components/aether/docs"

guides=(
    "aether_for_llms_and_others.md"
)

for name in "${guides[@]}"; do
    if [ ! -f "${aether_docs}/${name}" ]; then
        echo "[bundle aether docs] error: missing ${aether_docs}/${name}" >&2
        exit 1
    fi
done

dest="${bundle_root}/Docs/Aether"
/bin/rm -rf "${dest}"
/bin/mkdir -p "${dest}"
for name in "${guides[@]}"; do
    /bin/cp -f "${aether_docs}/${name}" "${dest}/${name}"
done

echo "[bundle aether docs] staged ${#guides[@]} file(s) into ${dest}"
