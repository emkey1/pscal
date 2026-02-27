#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ROOT_DIR="$(cd "${PROJECT_DIR}/.." && pwd)"

DEFAULT_OUTPUT="${SCRIPT_DIR}/micro.deflate"
INPUT=""
OUTPUT="${DEFAULT_OUTPUT}"

usage() {
  cat <<USAGE
Usage: $(basename "$0") [--input PATH] [--output PATH]

Create a zlib-deflated micro payload for iOS fallback packaging.

Options:
  --input PATH   Path to micro executable to compress.
                 If omitted, the script auto-searches common build outputs.
  --output PATH  Output .deflate path (default: ios/Tools/micro.deflate)
  -h, --help     Show this help

Auto-search order:
  1) build/ios-device/bin/micro
  2) build/ios-simulator/bin/micro
  3) build/ios-maccatalyst-arm64/bin/micro
  4) build/ios-maccatalyst/bin/micro
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

resolve_input() {
  if [[ -n "$INPUT" ]]; then
    if [[ "$INPUT" != /* ]]; then
      INPUT="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
    fi
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

  echo "error: no micro executable found; pass --input PATH" >&2
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
  OUTPUT="$(cd "$(dirname "$OUTPUT")" && pwd)/$(basename "$OUTPUT")"
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
