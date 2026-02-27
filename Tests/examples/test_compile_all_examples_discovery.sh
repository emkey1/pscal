#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_FILE="$(mktemp "${TMPDIR:-/tmp}/compile-all-discovery.XXXXXX")"
trap 'rm -f "$OUT_FILE"' EXIT

cd "$ROOT_DIR"
python3 Tests/examples/compile_all_examples.py \
    --languages pascal clike exsh rea \
    --list-only \
    --fail-on-empty >"$OUT_FILE"

for lang in pascal clike exsh rea; do
    if ! grep -Eq "^[[:space:]]+${lang}: [1-9][0-9]* source file\(s\)" "$OUT_FILE"; then
        echo "Missing or empty discovery count for ${lang}" >&2
        cat "$OUT_FILE" >&2
        exit 1
    fi
done

echo "PASS: compile_all_examples discovery reports non-zero counts for pascal/clike/exsh/rea"
