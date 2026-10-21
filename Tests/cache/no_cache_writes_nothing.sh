#!/bin/bash
# --no-cache neither reads nor writes the bytecode cache, in every front end:
# a --no-cache run leaves bc_cache empty, a plain run writes one entry, and a
# --no-cache run after that neither loads nor rewrites it.
# usage: PASCAL_BIN=... REA_BIN=... AETHER_BIN=... CLIKE_BIN=... EXSH_BIN=...
#        no_cache_writes_nothing.sh   (exit 0 = pass; a missing binary is skipped)
set -u
home=$(mktemp -d); src=$(mktemp -d)
trap 'rm -rf "$home" "$src"' EXIT
cache="$home/.pscal/bc_cache"
status=0
tested=0
fail() { echo "FAIL $1: $2"; status=1; }
entries() { ls "$cache" 2>/dev/null | wc -l | tr -d ' '; }

# check <name> <binary> <file> <expected output> <program text>
check() {
    local name=$1 bin=$2 file=$3 want=$4 text=$5 out
    if [ -z "$bin" ] || [ ! -x "$bin" ]; then
        echo "SKIP $name: binary not found ($bin)"
        return
    fi
    tested=$((tested + 1))
    rm -rf "$home/.pscal"
    printf '%s\n' "$text" > "$src/$file"
    touch -t 202001010000 "$src/$file"
    run() { (cd "$src" && HOME="$home" "$bin" "$@" "$file" 2>"$home/err"); }

    out=$(run --no-cache)
    [ "$out" = "$want" ] || { fail "$name" "--no-cache run printed '$out': $(cat "$home/err")"; return; }
    [ "$(entries)" = 0 ] || { fail "$name" "--no-cache wrote $(ls "$cache")"; return; }

    out=$(run)
    [ "$out" = "$want" ] || { fail "$name" "cached run printed '$out': $(cat "$home/err")"; return; }
    [ "$(entries)" = 1 ] || { fail "$name" "a plain run should write one entry: $(ls "$cache" 2>/dev/null)"; return; }

    sleep 1
    touch "$home/marker"
    sleep 1
    out=$(run --no-cache --verbose)
    [ "$out" = "$want" ] || { fail "$name" "second --no-cache run printed '$out': $(cat "$home/err")"; return; }
    grep -q 'Loaded cached bytecode' "$home/err" && { fail "$name" "--no-cache loaded the cache entry"; return; }
    [ "$(entries)" = 1 ] && [ -z "$(find "$cache" -type f -newer "$home/marker")" ] ||
        { fail "$name" "--no-cache rewrote the cache entry: $(ls -l "$cache")"; return; }
    echo "ok $name"
}

check pascal "${PASCAL_BIN:-}" NoCache.pas ok "program NoCache;
begin
  writeln('ok');
end."
check rea "${REA_BIN:-}" no_cache.rea ok 'writeln("ok");'
check aether "${AETHER_BIN:-}" no_cache.aether ok 'fn main() -> Void {
    fx { println("ok"); }
    ret;
}'
check clike "${CLIKE_BIN:-}" no_cache.cl ok 'int main() { printf("ok\n"); return 0; }'
check exsh "${EXSH_BIN:-}" no_cache.sh ok 'echo ok'

[ "$tested" -gt 0 ] || { echo "FAIL: no front end binary found"; exit 1; }
[ "$status" = 0 ] && echo PASS
exit $status
