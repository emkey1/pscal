#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

sdk_name="${SDK_NAME:-}"
preset=""
build_dir=""
arch=""

cmake_bin=""
cmake_candidates=()
if [[ -n "${CMAKE_BIN:-}" ]]; then
    cmake_candidates+=("${CMAKE_BIN}")
fi
cmake_candidates+=(
    "cmake"
    "/opt/homebrew/bin/cmake"
    "/usr/local/bin/cmake"
    "/Applications/CMake.app/Contents/bin/cmake"
    "/usr/bin/cmake"
)
for candidate in "${cmake_candidates[@]}"; do
    if [[ "${candidate}" == /* ]]; then
        if [[ -x "${candidate}" ]]; then
            cmake_bin="${candidate}"
            break
        fi
    else
        resolved="$(command -v "${candidate}" || true)"
        if [[ -n "${resolved}" && -x "${resolved}" ]]; then
            cmake_bin="${resolved}"
            break
        fi
    fi
done
if [[ -z "${cmake_bin}" ]]; then
    echo "[ios] error: unable to locate CMake executable (set CMAKE_BIN if needed)" >&2
    echo "[ios] checked: ${cmake_candidates[*]}" >&2
    exit 1
fi

case "${sdk_name}" in
    iphoneos*)
        preset="ios-device"
        build_dir="${root_dir}/build/ios-device"
        ;;
    iphonesimulator*)
        preset="ios-simulator"
        build_dir="${root_dir}/build/ios-simulator"
        ;;
    macosx*)
        arch="${NATIVE_ARCH_ACTUAL:-${CURRENT_ARCH:-}}"
        if [[ -z "${arch}" ]]; then
            arch="$(uname -m)"
        fi
        case "${arch}" in
            arm64|x86_64)
                ;;
            *)
                echo "[ios] error: unsupported macCatalyst arch '${arch}'" >&2
                exit 2
                ;;
        esac
        build_dir="${root_dir}/build/ios-maccatalyst-${arch}"
        ;;
    *)
        # Non-iOS SDKs do not use PSCAL iOS static archives.
        exit 0
        ;;
esac

required_libs=(
    libpscal_core_static.a
    libpscal_exsh_static.a
    libpscal_pascal_static.a
    libpscal_clike_static.a
    libpscal_rea_static.a
    libpscal_vm_static.a
    libpscal_json2bc_static.a
    libpscal_pscald_static.a
    libpscal_pscalasm_static.a
)

nm_tool=""
if command -v xcrun >/dev/null 2>&1; then
    nm_tool="$(xcrun -f nm 2>/dev/null || true)"
fi
if [[ -z "${nm_tool}" ]]; then
    nm_tool="$(command -v nm || true)"
fi
if [[ -z "${nm_tool}" ]]; then
    echo "[ios] error: unable to locate 'nm' for static-archive validation" >&2
    exit 1
fi

require_symbol_in_archive() {
    local archive_path="$1"
    local symbol_name="$2"
    local symbol_regex="^_?${symbol_name}$"
    if ! ( "${nm_tool}" -gU "${archive_path}" 2>/dev/null || true ) | /usr/bin/awk 'NF >= 3 && $2 != "U" {print $NF}' | /usr/bin/grep -E "${symbol_regex}" >/dev/null; then
        if ! ( "${nm_tool}" -g "${archive_path}" 2>/dev/null || true ) | /usr/bin/awk 'NF >= 3 && $2 != "U" {print $NF}' | /usr/bin/grep -E "${symbol_regex}" >/dev/null; then
            echo "[ios] error: required symbol '${symbol_name}' is missing from ${archive_path}" >&2
            exit 1
        fi
    fi
}

verify_required_artifacts() {
    for lib in "${required_libs[@]}"; do
        if [[ ! -f "${build_dir}/${lib}" ]]; then
            echo "[ios] error: missing required archive ${build_dir}/${lib}" >&2
            exit 1
        fi
    done

    local core_archive="${build_dir}/libpscal_core_static.a"
    require_symbol_in_archive "${core_archive}" "nextvi_main_entry"
    require_symbol_in_archive "${core_archive}" "pscal_micro_main_entry"
    require_symbol_in_archive "${core_archive}" "pscal_micro_go_main_entry"

    if /usr/bin/grep -q "nextvi" "${root_dir}/src/smallclue/src/micro_app.c"; then
        echo "[ios] error: src/smallclue/src/micro_app.c references nextvi; micro fallback is forbidden" >&2
        exit 1
    fi
}

if [[ -n "${preset}" ]]; then
    echo "[ios] ensuring static libs for ${sdk_name} via preset ${preset}"
    "${cmake_bin}" -S "${root_dir}" --preset "${preset}"
    "${cmake_bin}" --build "${build_dir}" --target \
        pscal_core_static \
        pscal_exsh_static \
        pscal_pascal_static \
        pscal_clike_static \
        pscal_rea_static \
        pscal_vm_static \
        pscal_json2bc_static \
        pscal_pscald_static \
        pscal_pscalasm_static \
        pscal_tool_runner
else
    echo "[ios] ensuring static libs for ${sdk_name}; building macCatalyst (${arch})"
    maccatalyst_flags="${PSCAL_MACCATALYST_CMAKE_FLAGS:-}"
    if [[ -z "${maccatalyst_flags}" ]]; then
        maccatalyst_flags="-DSDL=OFF"
    fi
    PSCAL_MACCATALYST_ARCH="${arch}" \
    PSCAL_MACCATALYST_CMAKE_FLAGS="${maccatalyst_flags}" \
    /bin/bash "${root_dir}/ios/Tools/build_maccatalyst_libs.sh"
fi

verify_required_artifacts
