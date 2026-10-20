#!/usr/bin/env bash
# Bytecode-cache layout and unit-search checks (Tests/cache): one entry per
# compiler and source path, written atomically in a 0700 directory; a unit
# search path longer than PATH_MAX is reported, not overrun.
set -u
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PASCAL_BIN="${PASCAL_BIN:-$ROOT_DIR/build/bin/pascal}"
status=0
for t in "$ROOT_DIR"/Tests/cache/*.sh; do
    name=$(basename "$t" .sh)
    if out=$(bash "$t" 2>&1); then
        echo "[PASS] cache_$name"
    else
        echo "[FAIL] cache_$name"
        echo "$out" | sed 's/^/    /'
        status=1
    fi
done
exit $status
