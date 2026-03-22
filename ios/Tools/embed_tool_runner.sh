#!/usr/bin/env bash
set -euo pipefail

bundle_root="${TARGET_BUILD_DIR}/${UNLOCALIZED_RESOURCES_FOLDER_PATH}"
/bin/mkdir -p "${bundle_root}"
configuration="${CONFIGURATION:-Debug}"
config_variant="$(printf '%s' "${configuration}" | /usr/bin/tr '[:upper:]' '[:lower:]')"

raw_dest="${bundle_root}/pscal_tool_runner"
compressed_dest="${bundle_root}/pscal_tool_runner.deflate"
license_dest="${bundle_root}/Docs/Licenses"

runner_src=""
case "${PLATFORM_NAME:-}" in
    iphoneos*)
        for candidate in "${PROJECT_DIR}/../build/ios-device-${config_variant}/bin/pscal_tool_runner.app/pscal_tool_runner" \
                         "${PROJECT_DIR}/../build/ios-device-${config_variant}/bin/pscal_tool_runner"; do
            if [ -f "${candidate}" ]; then
                runner_src="${candidate}"
                break
            fi
        done
        ;;
    iphonesimulator*)
        for candidate in "${PROJECT_DIR}/../build/ios-simulator-${config_variant}/bin/pscal_tool_runner.app/pscal_tool_runner" \
                         "${PROJECT_DIR}/../build/ios-simulator-${config_variant}/bin/pscal_tool_runner"; do
            if [ -f "${candidate}" ]; then
                runner_src="${candidate}"
                break
            fi
        done
        ;;
    *)
        for candidate in "${PROJECT_DIR}/../build/ios-maccatalyst-arm64/bin/pscal_tool_runner" \
                         "${PROJECT_DIR}/../build/ios-maccatalyst/bin/pscal_tool_runner"; do
            if [ -f "${candidate}" ]; then
                runner_src="${candidate}"
                break
            fi
        done
        ;;
esac

# Do not bundle the raw runner executable (App Store rejects extra binaries).
/bin/rm -f "${raw_dest}"

if [ -n "${runner_src}" ]; then
    /usr/bin/ruby -rzlib -e 'src, dst = ARGV; data = File.binread(src); File.binwrite(dst, Zlib::Deflate.deflate(data, Zlib::BEST_COMPRESSION))' "${runner_src}" "${compressed_dest}"
    echo "[pscal_tool_runner] compressed ${runner_src} -> ${compressed_dest}"
else
    echo "[pscal_tool_runner] error: missing pscal_tool_runner binary for ${PLATFORM_NAME:-unknown}" 1>&2
    exit 1
fi

if [ ! -s "${compressed_dest}" ]; then
    echo "[pscal_tool_runner] error: failed to write compressed payload ${compressed_dest}" 1>&2
    exit 1
fi

# Bundle license texts for the in-app viewer.
/bin/mkdir -p "${license_dest}"
/bin/cp -f "${PROJECT_DIR}/../LICENSE" "${license_dest}/pscal_LICENSE.txt" || true
/bin/cp -f "${PROJECT_DIR}/../third-party/openssl-3.6.0/LICENSE.txt" "${license_dest}/openssl_LICENSE.txt" || true
/bin/cp -f "${PROJECT_DIR}/../third-party/curl-8.17.0/COPYING" "${license_dest}/curl_LICENSE.txt" || true
/bin/cp -f "${PROJECT_DIR}/../third-party/SDL/LICENSE.txt" "${license_dest}/sdl_LICENSE.txt" || true
/bin/cp -f "${PROJECT_DIR}/../third-party/nextvi/LICENSE" "${license_dest}/nextvi_LICENSE.txt" || true
/bin/cp -f "${PROJECT_DIR}/../third-party/micro/LICENSE" "${license_dest}/micro_LICENSE.txt" || true
/bin/cp -f "${PROJECT_DIR}/../third-party/openssh-10.2p1/LICENCE" "${license_dest}/openssh_LICENSE.txt" || true
/bin/cp -f "${PROJECT_DIR}/../third-party/libgit2/COPYING" "${license_dest}/libgit2_LICENSE.txt" || true
/bin/cp -f "${PROJECT_DIR}/../src/third_party/yyjson/LICENSE" "${license_dest}/yyjson_LICENSE.txt" || true
/bin/cp -f "${PROJECT_DIR}/../third-party/hterm/LICENSE" "${license_dest}/hterm_LICENSE.txt" || true

exit 0
