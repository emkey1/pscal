#!/bin/bash
# The bytecode cache keeps one entry per compiler and source path, written
# atomically, in a 0700 directory, and a rewritten source replaces its entry.
# usage: PASCAL_BIN=... cache_layout.sh   (exit 0 = pass)
set -u
BIN=${PASCAL_BIN:?set PASCAL_BIN}
home=$(mktemp -d); src=$(mktemp -d)
trap 'rm -rf "$home" "$src"' EXIT
fail() { echo "FAIL: $*"; exit 1; }
cache="$home/.pscal/bc_cache"
write() { printf 'program CacheLayout;\nbegin\n  writeln(%s);\nend.\n' "$1" > "$src/CacheLayout"; touch -t 202001010000 "$src/CacheLayout"; }
run() { (cd "$src" && HOME="$home" "$BIN" --verbose CacheLayout 2>"$home/err") ; }

write "'one'"
[ "$(run)" = one ] || fail "first run"
[ "$(stat -f %Lp "$cache" 2>/dev/null || stat -c %a "$cache")" = 700 ] || fail "bc_cache is not 0700"
[ "$(ls "$cache" | wc -l | tr -d ' ')" = 1 ] || fail "expected one entry: $(ls "$cache")"
sleep 1
[ "$(run)" = one ] || fail "second run"
grep -q 'Loaded cached bytecode' "$home/err" || fail "second run did not load the cache: $(cat "$home/err")"
for v in two three four; do
    write "'$v'"
    [ "$(run)" = "$v" ] || fail "run after rewriting the source to '$v'"
done
[ "$(ls "$cache" | wc -l | tr -d ' ')" = 1 ] || fail "edits left more than one entry: $(ls "$cache")"
ls "$cache" | grep -q '\.tmp\.' && fail "temporary file left behind"
sleep 1
[ "$(run)" = four ] || fail "run of the last version"
grep -q 'Loaded cached bytecode' "$home/err" || fail "last version did not load from the cache"
echo PASS
