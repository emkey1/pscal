#!/usr/bin/env bash
#
# Stage the Tiny compiler into the app bundle: the bin/tiny wrapper, the
# bin/tiny.clike compiler it runs under clike, and the pscal-core files
# tiny.clike reads at compile time. RuntimeAssetInstaller mirrors bin/ and
# src/ from here into the workspace, where tiny.clike finds the headers under
# $PSCALI_WORKSPACE_ROOT/src. This is the iOS counterpart of the "Tiny
# compiler runtime assets" install rules in CMakeLists.txt; keep the two
# lists in step.
#
# The app carries no other copy of these files, so a missing one fails the
# build. tiny.pbc is not bundled: the Xcode build has no host clike to produce
# it, and without it bin/tiny runs tiny.clike under clike, which costs about
# the same per compile.
#
# Usage: bundle_tiny_assets.sh <bundle_root>
set -euo pipefail

bundle_root="${1:?usage: bundle_tiny_assets.sh <bundle_root>}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
core_src="${repo_root}/components/pscal-core/src"

# <path in bundle>|<source>
assets=(
    "bin/tiny|${repo_root}/bin/tiny"
    "bin/tiny.clike|${repo_root}/bin/tiny.clike"
    "src/compiler/bytecode.h|${core_src}/compiler/bytecode.h"
    "src/compiler/opcodes.def|${core_src}/compiler/opcodes.def"
    "src/core/version.h|${core_src}/core/version.h"
    "src/core/var_type.h|${core_src}/core/var_type.h"
)

missing=0
for spec in "${assets[@]}"; do
    src="${spec#*|}"
    if [ ! -f "${src}" ]; then
        echo "[bundle tiny] error: missing ${src}" >&2
        missing=1
    fi
done
if [ "${missing}" -ne 0 ]; then
    exit 1
fi

# Rebuild both directories from scratch so a file dropped from the list does
# not linger in an incrementally built bundle.
/bin/rm -rf "${bundle_root}/bin" "${bundle_root}/src"
for spec in "${assets[@]}"; do
    dest="${bundle_root}/${spec%%|*}"
    /bin/mkdir -p "$(dirname "${dest}")"
    /bin/cp -f "${spec#*|}" "${dest}"
done
/bin/chmod 0755 "${bundle_root}/bin/tiny"

echo "[bundle tiny] staged ${#assets[@]} file(s) into ${bundle_root}/bin and ${bundle_root}/src"
