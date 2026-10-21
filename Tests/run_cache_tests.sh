#!/usr/bin/env bash
# Bytecode-cache layout and unit-search checks (Tests/cache): one entry per
# compiler and source path, written atomically in a 0700 directory; a unit
# search path longer than PATH_MAX is reported, not overrun; --no-cache
# neither reads nor writes the cache in any front end.
set -u
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PASCAL_BIN="${PASCAL_BIN:-$ROOT_DIR/build/bin/pascal}"
export REA_BIN="${REA_BIN:-$ROOT_DIR/build/bin/rea}"
export AETHER_BIN="${AETHER_BIN:-$ROOT_DIR/build/bin/aether}"
export CLIKE_BIN="${CLIKE_BIN:-$ROOT_DIR/build/bin/clike}"
export EXSH_BIN="${EXSH_BIN:-$ROOT_DIR/build/bin/exsh}"
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
