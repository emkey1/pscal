#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${PSCAL_BIN_DIR:-${ROOT}/build/bin}"

FRONTENDS=(pascal rea clike)
REQUIRED_ENTRIES=(
  "category graphics"
  "category 3d"
  "function 3d physics BouncingBalls3DStepUltraAdvanced"
  "function 3d rendering BouncingBalls3DDrawUnitSphereFast"
  "function graphics window InitGraph3D"
  "function graphics window GraphLoop"
  "function graphics textures RenderCopyEx"
  "function graphics input PollKeyAny"
  "function graphics opengl GLIsHardwareAccelerated"
)

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ios-ext-builtins.XXXXXX")"
trap 'rm -rf "${TMP_DIR}"' EXIT

echo "Running iOS extended builtin sanity checks..."

for frontend in "${FRONTENDS[@]}"; do
  bin_path="${BIN_DIR}/${frontend}"
  if [ ! -x "${bin_path}" ]; then
    echo "error: missing frontend binary ${bin_path}" >&2
    exit 1
  fi

  out_file="${TMP_DIR}/${frontend}.txt"
  "${bin_path}" --dump-ext-builtins > "${out_file}"

  if ! grep -Fqx "category system" "${out_file}"; then
    echo "error: ${frontend} missing category system in --dump-ext-builtins output" >&2
    exit 1
  fi

  for entry in "${REQUIRED_ENTRIES[@]}"; do
    if ! grep -Fqx "${entry}" "${out_file}"; then
      echo "error: ${frontend} missing required extended builtin entry: ${entry}" >&2
      exit 1
    fi
  done

  echo "  ${frontend}: required extended builtins present"
done

base_frontend="${FRONTENDS[0]}"
base_file="${TMP_DIR}/${base_frontend}.txt"
for frontend in "${FRONTENDS[@]:1}"; do
  compare_file="${TMP_DIR}/${frontend}.txt"
  if ! cmp -s "${base_file}" "${compare_file}"; then
    echo "error: extended builtin inventory mismatch: ${base_frontend} vs ${frontend}" >&2
    diff -u "${base_file}" "${compare_file}" || true
    exit 1
  fi
done

echo "Extended builtin inventories are consistent across: ${FRONTENDS[*]}"
