#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${PROJECT_DIR}/.." && pwd)"

DEFAULT_OUTPUT="${SCRIPT_DIR}/micro.deflate"
DEFAULT_GO_BIN="/opt/homebrew/bin/go"

INPUT=""
OUTPUT="${DEFAULT_OUTPUT}"
GO_BIN="${GO_BIN:-${DEFAULT_GO_BIN}}"
TARGET_GOOS=""
TARGET_GOARCH=""
TARGET_SDK=""

usage() {
  cat <<USAGE
Usage: $(basename "$0") [--input PATH] [--output PATH] [--go-bin PATH]
                          [--goos GOOS --goarch GOARCH [--sdk SDK]]

Create a zlib-deflated micro payload for iOS fallback packaging.

Options:
  --input PATH     Path to micro executable to compress.
                   If omitted, the script auto-searches common build outputs.
                   If still missing, it attempts to build micro from source.
  --output PATH    Output .deflate path (default: ios/Tools/micro.deflate)
  --go-bin PATH    Go binary to use when building from source.
  --goos GOOS      Override target GOOS for source build (for example: ios).
  --goarch GOARCH  Override target GOARCH for source build (arm64 or amd64).
  --sdk SDK        Override xcrun SDK for cgo cross-build (iphoneos/iphonesimulator/macosx).
  -h, --help       Show this help

Auto-search order:
  1) build/ios-device/bin/micro
  2) build/ios-simulator/bin/micro
  3) build/ios-maccatalyst-arm64/bin/micro
  4) build/ios-maccatalyst/bin/micro

Source-build defaults:
  - In Xcode builds (PLATFORM_NAME/CURRENT_ARCH set), target follows that platform.
  - Outside Xcode, target defaults to host darwin architecture.
USAGE
}

require_file() {
  local path="$1"
  if [[ ! -f "$path" ]]; then
    echo "error: file not found: $path" >&2
    exit 1
  fi
}

is_executable_file() {
  local path="$1"
  [[ -f "$path" && -x "$path" ]]
}

to_abs_path() {
  local path="$1"
  if [[ "$path" == /* ]]; then
    printf '%s\n' "$path"
  else
    printf '%s/%s\n' "$(cd "$(dirname "$path")" && pwd)" "$(basename "$path")"
  fi
}

map_go_arch() {
  case "$1" in
    arm64|aarch64)
      echo "arm64"
      ;;
    amd64|x86_64)
      echo "amd64"
      ;;
    *)
      return 1
      ;;
  esac
}

resolve_go_target() {
  if [[ -n "$TARGET_GOOS" || -n "$TARGET_GOARCH" ]]; then
    if [[ -z "$TARGET_GOOS" || -z "$TARGET_GOARCH" ]]; then
      echo "error: --goos and --goarch must be provided together" >&2
      exit 1
    fi
    return
  fi

  local platform="${PLATFORM_NAME:-}"
  local current_arch="${CURRENT_ARCH:-}"
  local host_arch
  host_arch="$(uname -m)"

  case "$platform" in
    iphoneos*)
      TARGET_GOOS="ios"
      TARGET_GOARCH="arm64"
      TARGET_SDK="${TARGET_SDK:-iphoneos}"
      ;;
    iphonesimulator*)
      TARGET_GOOS="ios"
      TARGET_GOARCH="$(map_go_arch "${current_arch:-$host_arch}")" || {
        echo "error: unsupported simulator arch: ${current_arch:-$host_arch}" >&2
        exit 1
      }
      TARGET_SDK="${TARGET_SDK:-iphonesimulator}"
      ;;
    *)
      TARGET_GOOS="darwin"
      TARGET_GOARCH="$(map_go_arch "${current_arch:-$host_arch}")" || {
        echo "error: unsupported host arch: ${current_arch:-$host_arch}" >&2
        exit 1
      }
      TARGET_SDK="${TARGET_SDK:-macosx}"
      ;;
  esac
}

build_micro_from_source() {
  local src_dir="${ROOT_DIR}/third-party/micro"
  if [[ ! -d "$src_dir" ]]; then
    echo "warning: micro source directory missing: $src_dir" >&2
    return 1
  fi

  if [[ "$GO_BIN" != /* ]]; then
    GO_BIN="$(command -v "$GO_BIN" || true)"
  fi
  if [[ -z "$GO_BIN" || ! -x "$GO_BIN" ]]; then
    echo "warning: Go toolchain not found; cannot build micro from source." >&2
    return 1
  fi

  resolve_go_target

  local build_root="${ROOT_DIR}/build/go-micro"
  local gopath="${build_root}/gopath"
  local gomodcache="${gopath}/pkg/mod"
  local gocache="${build_root}/cache"
  local out_bin="${build_root}/micro-${TARGET_GOOS}-${TARGET_GOARCH}"

  mkdir -p "$gomodcache" "$gocache"

  local -a env_cmd=(
    env -u GOROOT
    "GOPATH=${gopath}"
    "GOMODCACHE=${gomodcache}"
    "GOCACHE=${gocache}"
    "GOOS=${TARGET_GOOS}"
    "GOARCH=${TARGET_GOARCH}"
  )

  if [[ "$TARGET_GOOS" == "ios" ]]; then
    local cc sdk_path
    cc="$(xcrun --sdk "$TARGET_SDK" --find clang 2>/dev/null || true)"
    sdk_path="$(xcrun --sdk "$TARGET_SDK" --show-sdk-path 2>/dev/null || true)"
    if [[ -z "$cc" || -z "$sdk_path" ]]; then
      echo "warning: failed to resolve SDK toolchain for ${TARGET_SDK}; cannot build iOS micro." >&2
      return 1
    fi
    env_cmd+=(
      "CGO_ENABLED=1"
      "CC=${cc}"
      "CGO_CFLAGS=-isysroot ${sdk_path}"
      "CGO_LDFLAGS=-isysroot ${sdk_path}"
    )
  else
    env_cmd+=("CGO_ENABLED=0")
  fi

  echo "building micro from source (${TARGET_GOOS}/${TARGET_GOARCH})..."
  if ! (
    cd "$src_dir"
    "${env_cmd[@]}" "$GO_BIN" build -trimpath -ldflags "-s -w" -o "$out_bin" ./cmd/micro
  ); then
    echo "warning: micro source build failed." >&2
    return 1
  fi

  if ! is_executable_file "$out_bin"; then
    echo "warning: micro source build did not produce executable output." >&2
    return 1
  fi

  INPUT="$out_bin"
  return 0
}

resolve_input() {
  if [[ -n "$INPUT" ]]; then
    INPUT="$(to_abs_path "$INPUT")"
    return
  fi

  local candidates=(
    "${ROOT_DIR}/build/ios-device/bin/micro"
    "${ROOT_DIR}/build/ios-simulator/bin/micro"
    "${ROOT_DIR}/build/ios-maccatalyst-arm64/bin/micro"
    "${ROOT_DIR}/build/ios-maccatalyst/bin/micro"
  )

  local c
  for c in "${candidates[@]}"; do
    if is_executable_file "$c"; then
      INPUT="$c"
      return
    fi
  done

  if build_micro_from_source; then
    return
  fi

  echo "error: no micro executable found; pass --input PATH or install/build Go toolchain." >&2
  exit 1
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input)
      [[ $# -ge 2 ]] || { echo "error: --input requires a value" >&2; exit 1; }
      INPUT="$2"
      shift 2
      ;;
    --output)
      [[ $# -ge 2 ]] || { echo "error: --output requires a value" >&2; exit 1; }
      OUTPUT="$2"
      shift 2
      ;;
    --go-bin)
      [[ $# -ge 2 ]] || { echo "error: --go-bin requires a value" >&2; exit 1; }
      GO_BIN="$2"
      shift 2
      ;;
    --goos)
      [[ $# -ge 2 ]] || { echo "error: --goos requires a value" >&2; exit 1; }
      TARGET_GOOS="$2"
      shift 2
      ;;
    --goarch)
      [[ $# -ge 2 ]] || { echo "error: --goarch requires a value" >&2; exit 1; }
      TARGET_GOARCH="$2"
      shift 2
      ;;
    --sdk)
      [[ $# -ge 2 ]] || { echo "error: --sdk requires a value" >&2; exit 1; }
      TARGET_SDK="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

resolve_input
require_file "$INPUT"

if [[ "$OUTPUT" != /* ]]; then
  OUTPUT="$(to_abs_path "$OUTPUT")"
fi
mkdir -p "$(dirname "$OUTPUT")"

if ! command -v ruby >/dev/null 2>&1; then
  echo "error: ruby is required to generate zlib payload" >&2
  exit 1
fi

ruby -rzlib -e 'src, dst = ARGV; data = File.binread(src); File.binwrite(dst, Zlib::Deflate.deflate(data, Zlib::BEST_COMPRESSION))' "$INPUT" "$OUTPUT"

BYTES_IN=$(wc -c < "$INPUT" | tr -d ' ')
BYTES_OUT=$(wc -c < "$OUTPUT" | tr -d ' ')
echo "micro payload created"
echo "  input : $INPUT (${BYTES_IN} bytes)"
echo "  output: $OUTPUT (${BYTES_OUT} bytes)"
