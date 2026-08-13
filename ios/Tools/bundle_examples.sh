#!/usr/bin/env bash
#
# Aggregate the per-component example programs into the app bundle's Examples/
# directory. The repo split (monorepo -> components/<lang>) moved the example
# sources out of the top-level Examples/ folder reference that Xcode bundles,
# so without this step the shipped app only contains a few stragglers.
#
# Layout: components/<lang>/Examples/<...> -> <bundle>/Examples/<lang>/<...>
# which reproduces the original monolithic Examples/<lang>/ layout that the
# RuntimeAssetInstaller seeds into ~/Examples.
#
# Usage: bundle_examples.sh <bundle_root>
set -euo pipefail

bundle_root="${1:?usage: bundle_examples.sh <bundle_root>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
dest_root="${bundle_root}/Examples"

mkdir -p "${dest_root}"

# Every front-end component that ships example programs.
components=(pascal clike rea aether exsh)

rsync_excludes=(
    --exclude='.DS_Store'
    --exclude='.git*'
    --exclude='*.save'
    --exclude='*.sav'
    --exclude='.tmp_*'
    --exclude='typescript'
    --exclude='CMakeLists.txt'
    --exclude='Makefile'
)

copied_any=0
for comp in "${components[@]}"; do
    src="${repo_root}/components/${comp}/Examples"
    if [ ! -d "${src}" ]; then
        echo "[bundle examples] note: no Examples in components/${comp}; skipping" >&2
        continue
    fi
    /usr/bin/rsync -a "${rsync_excludes[@]}" "${src}/" "${dest_root}/${comp}/"
    copied_any=1
done

if [ "${copied_any}" -eq 0 ]; then
    echo "[bundle examples] warning: no component examples were found to bundle" >&2
fi

count="$(/usr/bin/find "${dest_root}" -type f 2>/dev/null | /usr/bin/wc -l | /usr/bin/tr -d ' ')"
echo "[bundle examples] Examples/ now holds ${count} file(s) at ${dest_root}"
