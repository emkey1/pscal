#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"

arch="${PSCAL_MACCATALYST_ARCH:-${CURRENT_ARCH:-}}"
if [[ -z "${arch}" ]]; then
  arch="$(uname -m)"
fi

deployment="${MACCATALYST_DEPLOYMENT_TARGET:-${IPHONEOS_DEPLOYMENT_TARGET:-15.0}}"
build_dir="${PSCAL_MACCATALYST_BUILD_DIR:-${ROOT_DIR}/build/ios-maccatalyst-${arch}}"

case "${arch}" in
  arm64|x86_64) ;;
  *)
    echo "[maccatalyst] error: unsupported arch '${arch}' (expected arm64 or x86_64)" >&2
    exit 2
    ;;
esac

cflags="-target ${arch}-apple-ios${deployment}-macabi"

echo "[maccatalyst] arch=${arch} deployment=${deployment} build_dir=${build_dir}"
cmake -S "${ROOT_DIR}" -B "${build_dir}" \
  -DCMAKE_OSX_SYSROOT=macosx \
  -DCMAKE_OSX_ARCHITECTURES="${arch}" \
  -DCMAKE_C_FLAGS="${cflags}" \
  -DCMAKE_CXX_FLAGS="${cflags}" \
  -DPSCAL_FORCE_IOS=ON \
  -DPSCAL_BUILD_STATIC_LIBS=ON \
  ${PSCAL_MACCATALYST_CMAKE_FLAGS:-}
cmake --build "${build_dir}"
