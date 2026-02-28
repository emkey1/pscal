#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MICRO_DIR="${ROOT_DIR}/third-party/micro"

GO_BIN="${GO_BIN:-go}"
OUTPUT_LIB=""
OUTPUT_HEADER=""
TARGET_GOOS=""
TARGET_GOARCH=""
TARGET_SDK=""
TARGET_TRIPLE=""

usage() {
  cat <<USAGE
Usage: $(basename "$0") --output-lib PATH --output-header PATH
       [--go-bin PATH] --goos GOOS --goarch GOARCH --sdk SDK --target TARGET

Builds the embedded micro archive used by iOS/iPadOS PSCAL builds.

Required:
  --output-lib PATH      Output archive path (.a)
  --output-header PATH   Output header path (.h)
  --goos GOOS            Go target OS (ios or darwin)
  --goarch GOARCH        Go target arch (arm64 or amd64)
  --sdk SDK              xcrun SDK (iphoneos, iphonesimulator, macosx)
  --target TARGET        Clang target triple (for cgo)

Optional:
  --go-bin PATH          Go tool binary (default: go from PATH)
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-lib)
      OUTPUT_LIB="$2"
      shift 2
      ;;
    --output-header)
      OUTPUT_HEADER="$2"
      shift 2
      ;;
    --go-bin)
      GO_BIN="$2"
      shift 2
      ;;
    --goos)
      TARGET_GOOS="$2"
      shift 2
      ;;
    --goarch)
      TARGET_GOARCH="$2"
      shift 2
      ;;
    --sdk)
      TARGET_SDK="$2"
      shift 2
      ;;
    --target)
      TARGET_TRIPLE="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[micro-embed] error: unknown argument '$1'" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "${OUTPUT_LIB}" || -z "${OUTPUT_HEADER}" || -z "${TARGET_GOOS}" || -z "${TARGET_GOARCH}" || -z "${TARGET_SDK}" || -z "${TARGET_TRIPLE}" ]]; then
  echo "[micro-embed] error: missing required arguments" >&2
  usage >&2
  exit 2
fi

if [[ ! -d "${MICRO_DIR}" ]]; then
  echo "[micro-embed] error: missing micro source directory: ${MICRO_DIR}" >&2
  exit 1
fi

go_bin_resolved=""
go_candidates=("${GO_BIN}")
if [[ "${GO_BIN}" != "/opt/homebrew/bin/go" ]]; then
  go_candidates+=("/opt/homebrew/bin/go")
fi
if [[ "${GO_BIN}" != "/usr/local/go/bin/go" ]]; then
  go_candidates+=("/usr/local/go/bin/go")
fi
if [[ "${GO_BIN}" != "/usr/local/bin/go" ]]; then
  go_candidates+=("/usr/local/bin/go")
fi
for candidate in "${go_candidates[@]}"; do
  if [[ "${candidate}" == /* ]]; then
    if [[ -x "${candidate}" ]]; then
      go_bin_resolved="${candidate}"
      break
    fi
  else
    resolved="$(command -v "${candidate}" || true)"
    if [[ -n "${resolved}" && -x "${resolved}" ]]; then
      go_bin_resolved="${resolved}"
      break
    fi
  fi
done
GO_BIN="${go_bin_resolved}"
if [[ -z "${GO_BIN}" || ! -x "${GO_BIN}" ]]; then
  echo "[micro-embed] error: go tool not found" >&2
  echo "[micro-embed] checked: ${go_candidates[*]}" >&2
  exit 1
fi

CC_PATH="$(xcrun --sdk "${TARGET_SDK}" --find clang 2>/dev/null || true)"
SDK_PATH="$(xcrun --sdk "${TARGET_SDK}" --show-sdk-path 2>/dev/null || true)"
if [[ -z "${CC_PATH}" || -z "${SDK_PATH}" ]]; then
  echo "[micro-embed] error: failed to resolve clang/sdk for ${TARGET_SDK}" >&2
  exit 1
fi

mkdir -p "$(dirname "${OUTPUT_LIB}")" "$(dirname "${OUTPUT_HEADER}")"

build_root="${ROOT_DIR}/build/go-micro-embed"
gopath="${build_root}/gopath"
gomodcache="${gopath}/pkg/mod"
gocache="${build_root}/cache"
gotmp="${build_root}/tmp"
mkdir -p "${gomodcache}" "${gocache}" "${gotmp}"

go_env_base=(
  env -u GOROOT
  "GOPATH=${gopath}"
  "GOMODCACHE=${gomodcache}"
  "GOCACHE=${gocache}"
  "TMPDIR=${gotmp}"
)

apply_tcell_ios_patch() {
  local patch_darwin="${ROOT_DIR}/ios/Tools/go_patches/tcell_tscreen_unix_darwin.go"
  local patch_unix="${ROOT_DIR}/ios/Tools/go_patches/tcell_tscreen_unix.go"
  if [[ ! -f "${patch_darwin}" ]]; then
    echo "[micro-embed] error: missing tcell iOS patch source: ${patch_darwin}" >&2
    exit 1
  fi
  if [[ ! -f "${patch_unix}" ]]; then
    echo "[micro-embed] error: missing tcell iOS patch source: ${patch_unix}" >&2
    exit 1
  fi

  local tcell_dir=""
  (
    cd "${MICRO_DIR}"
    "${go_env_base[@]}" "${GO_BIN}" mod download github.com/micro-editor/tcell/v2 >/dev/null
    tcell_dir="$("${go_env_base[@]}" "${GO_BIN}" list -m -f '{{.Dir}}' github.com/micro-editor/tcell/v2 2>/dev/null || true)"
    if [[ -z "${tcell_dir}" || ! -f "${tcell_dir}/tscreen_unix_darwin.go" ]]; then
      echo "[micro-embed] error: unable to locate tcell module dir for iOS patching" >&2
      exit 1
    fi
    if [[ ! -f "${tcell_dir}/tscreen_unix.go" ]]; then
      echo "[micro-embed] error: unable to locate tscreen_unix.go for iOS patching" >&2
      exit 1
    fi
    chmod u+w "${tcell_dir}/tscreen_unix_darwin.go" 2>/dev/null || true
    chmod u+w "${tcell_dir}/tscreen_unix.go" 2>/dev/null || true
    chmod u+w "${tcell_dir}/tscreen.go" 2>/dev/null || true
    cp -f "${patch_darwin}" "${tcell_dir}/tscreen_unix_darwin.go"
    cp -f "${patch_unix}" "${tcell_dir}/tscreen_unix.go"

    # Guard t.quit close in tcell finish() for partial-init shutdown paths.
    local tscreen_file="${tcell_dir}/tscreen.go"
    if ! grep -q "close(t.quit)" "${tscreen_file}"; then
      echo "[micro-embed] error: expected close(t.quit) in ${tscreen_file}" >&2
      exit 1
    fi
    local patched_tscreen="${gotmp}/tcell_tscreen.go.patched"
    perl -0777 -pe 's/select \{\n\tcase <-t\.quit:\n\t\t\/\/ do nothing, already closed\n\n\tdefault:\n\t\tclose\(t\.quit\)\n\t\}/if t.quit != nil {\n\t\tselect {\n\t\tcase <-t.quit:\n\t\t\t\/\/ do nothing, already closed\n\n\t\tdefault:\n\t\t\tclose(t.quit)\n\t\t}\n\t}/s' "${tscreen_file}" > "${patched_tscreen}"
    cp -f "${patched_tscreen}" "${tscreen_file}"
    if ! grep -q "if t.quit != nil {" "${tscreen_file}"; then
      echo "[micro-embed] error: failed to patch tcell quit-guard in ${tscreen_file}" >&2
      exit 1
    fi
  )
}

tmp_header="${OUTPUT_LIB%.a}.h"

echo "[micro-embed] building GOOS=${TARGET_GOOS} GOARCH=${TARGET_GOARCH} SDK=${TARGET_SDK} TARGET=${TARGET_TRIPLE}"
apply_tcell_ios_patch
(
  cd "${MICRO_DIR}"
  "${go_env_base[@]}" \
    "GOOS=${TARGET_GOOS}" \
    "GOARCH=${TARGET_GOARCH}" \
    "CGO_ENABLED=1" \
    "CC=${CC_PATH}" \
    "CGO_CFLAGS=-isysroot ${SDK_PATH} -target ${TARGET_TRIPLE}" \
    "CGO_LDFLAGS=-isysroot ${SDK_PATH} -target ${TARGET_TRIPLE}" \
    "${GO_BIN}" build \
      -tags pscal_embed \
      -trimpath \
      -buildmode=c-archive \
      -o "${OUTPUT_LIB}" \
      ./cmd/micro
)

if [[ ! -f "${OUTPUT_LIB}" ]]; then
  echo "[micro-embed] error: archive not created: ${OUTPUT_LIB}" >&2
  exit 1
fi
if [[ ! -f "${tmp_header}" ]]; then
  echo "[micro-embed] error: header not created: ${tmp_header}" >&2
  exit 1
fi

if [[ "${tmp_header}" != "${OUTPUT_HEADER}" ]]; then
  cp -f "${tmp_header}" "${OUTPUT_HEADER}"
fi

echo "[micro-embed] wrote ${OUTPUT_LIB}"
echo "[micro-embed] wrote ${OUTPUT_HEADER}"
