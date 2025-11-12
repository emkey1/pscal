#!/usr/bin/env bash
set -euo pipefail

# PSCAL iOS front-ends core de-duplication script
#
# Purpose:
# - Ensures only one copy of the shared core (parser.c.o, compiler.c.o, ast.c.o, etc.)
#   is present across libpscal_pascal_static.a and libpscal_rea_static.a.
# - Removes any overlapping object files from one of the two libraries so the app can
#   link both front-ends without duplicate symbols.
#
# Environment variables:
#   KEEP_CORE_IN   : 'rea' (default) or 'pascal' — which library should retain the core
#   PSCAL_LIB_DIR  : Directory containing libpscal_pascal_static.a and libpscal_rea_static.a
#                    Defaults to the repo's ../build/<platform> relative to this script.
#   DRY_RUN        : If set to a non-empty value, prints planned actions without modifying archives.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IOS_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${IOS_DIR}/.." && pwd)"

sdk="${SDK_NAME:-}"
case "$sdk" in
  iphonesimulator*) platform_dir="ios-simulator" ;;
  iphoneos*)        platform_dir="ios-device" ;;
  *)                platform_dir="ios-simulator" ;;
esac
DEFAULT_LIB_DIR="${ROOT_DIR}/build/${platform_dir}"

KEEP_CORE_IN="${KEEP_CORE_IN:-rea}"
LIB_DIR="${PSCAL_LIB_DIR:-${DEFAULT_LIB_DIR}}"

PAS_LIB="${LIB_DIR}/libpscal_pascal_static.a"
REA_LIB="${LIB_DIR}/libpscal_rea_static.a"

if [[ ! -f "${PAS_LIB}" ]]; then
  echo "[pscal-dedupe] Skipping: missing ${PAS_LIB}" >&2
  exit 0
fi
if [[ ! -f "${REA_LIB}" ]]; then
  echo "[pscal-dedupe] Skipping: missing ${REA_LIB}" >&2
  exit 0
fi

command -v ar >/dev/null 2>&1 || { echo "[pscal-dedupe] error: 'ar' not found" >&2; exit 1; }
command -v ranlib >/dev/null 2>&1 || { echo "[pscal-dedupe] error: 'ranlib' not found" >&2; exit 1; }

pas_tmp="$(mktemp)"; rea_tmp="$(mktemp)"; common_tmp="$(mktemp)"
trap 'rm -f "${pas_tmp}" "${rea_tmp}" "${common_tmp}"' EXIT
ar -t "${PAS_LIB}" | sort -u > "${pas_tmp}"
ar -t "${REA_LIB}" | sort -u > "${rea_tmp}"
comm -12 "${pas_tmp}" "${rea_tmp}" > "${common_tmp}"

if [[ ! -s "${common_tmp}" ]]; then
  echo "[pscal-dedupe] No overlapping objects found. Nothing to do."
  exit 0
fi

echo "[pscal-dedupe] Found overlapping objects:" >&2
while IFS= read -r obj; do
  echo "  - ${obj}" >&2
done < "${common_tmp}"

case "${KEEP_CORE_IN}" in
  rea|REA)
    strip_from="${PAS_LIB}"
    echo "[pscal-dedupe] Keeping core in REA; stripping duplicates from Pascal archive." >&2
    ;;
  pascal|PASCAL)
    strip_from="${REA_LIB}"
    echo "[pscal-dedupe] Keeping core in PASCAL; stripping duplicates from REA archive." >&2
    ;;
  *)
    echo "[pscal-dedupe] error: KEEP_CORE_IN must be 'rea' or 'pascal' (got '${KEEP_CORE_IN}')" >&2
    exit 2
    ;;
esac

if [[ -n "${DRY_RUN:-}" ]]; then
  echo "[pscal-dedupe] DRY_RUN set — no changes will be made." >&2
fi

while IFS= read -r obj; do
  if [[ -n "${DRY_RUN:-}" ]]; then
    echo "ar -d \"${strip_from}\" \"${obj}\"" >&2
  else
    if ! ar -d "${strip_from}" "${obj}" >/dev/null 2>&1; then
      echo "[pscal-dedupe] note: object '${obj}' not found in $(basename "${strip_from}") (already stripped?)" >&2
    fi
  fi
done < "${common_tmp}"

if [[ -z "${DRY_RUN:-}" ]]; then
  ranlib "${strip_from}"
  echo "[pscal-dedupe] Updated archive index: $(basename "${strip_from}")" >&2
fi

echo "[pscal-dedupe] Done." >&2
