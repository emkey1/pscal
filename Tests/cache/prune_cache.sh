#!/bin/bash
# --prune-cache removes exactly the cache entries whose recorded source file no
# longer exists, whichever compiler and container format wrote them, and keeps
# everything it cannot judge: unreadable files, entries from a newer format,
# sources stat() cannot answer for. Symlinks and other files are not entries.
# usage: PASCAL_BIN=... [REA_BIN= AETHER_BIN= CLIKE_BIN= EXSH_BIN=] prune_cache.sh
#        (exit 0 = pass; each optional front end is skipped when missing)
set -u
BIN=${PASCAL_BIN:?set PASCAL_BIN}
root=$(mktemp -d)
trap 'chmod -R u+rwx "$root" 2>/dev/null; rm -rf "$root"' EXIT
home="$root/home"; src="$root/src"; cache="$home/.pscal/bc_cache"
mkdir -p "$home" "$src/locked"
fail() { echo "FAIL: $*"; exit 1; }
pas() { printf "program %s;\nbegin\n  writeln('%s');\nend.\n" "$1" "$1" > "$2"; touch -t 202001010000 "$2"; }
entry() { ls "$cache"/pascal-"$1".pas-*.bc 2>/dev/null; }
# The container format is the u16 at byte 4; its low byte is enough here.
set_format() { printf "\\x$(printf %02x "$2")" | dd of="$1" bs=1 seek=4 conv=notrunc 2>/dev/null; }

pas Keep "$src/Keep.pas"
pas Gone "$src/Gone.pas"
pas Old "$src/Old.pas"
pas Locked "$src/locked/Locked.pas"
for p in Keep.pas Gone.pas Old.pas locked/Locked.pas; do
    (cd "$src" && HOME="$home" "$BIN" "$p" > /dev/null) || fail "compiling $p"
done
[ "$(ls "$cache" | wc -l | tr -d ' ')" = 4 ] || fail "expected 4 entries: $(ls "$cache")"
set_format "$(entry Old)" 5            # written by an older build
printf junk > "$cache/junk.bc"
cp "$(entry Keep)" "$cache/newer.bc"
set_format "$cache/newer.bc" 255       # written by a newer build
ln -s "$src/Gone.pas" "$cache/link.bc"
touch "$cache/notes.txt" "$cache/x.bc.tmp.1"
rm "$src/Gone.pas" "$src/Old.pas"
check_locked=1
chmod 000 "$src/locked"
[ -r "$src/locked" ] && check_locked=0  # root reads it anyway

out=$(HOME="$home" "$BIN" --prune-cache --verbose 2>&1)
rc=$?
chmod 755 "$src/locked"
[ $rc = 0 ] || fail "--prune-cache exited $rc: $out"
[ -z "$(entry Gone)" ] || fail "the entry for a deleted source survived"
[ -z "$(entry Old)" ] || fail "an older-format entry for a deleted source survived"
[ -n "$(entry Keep)" ] || fail "the entry for an existing source was removed"
[ -n "$(entry Locked)" ] || fail "an entry whose source could not be checked was removed"
for f in junk.bc newer.bc notes.txt x.bc.tmp.1; do
    [ -e "$cache/$f" ] || fail "$f was removed"
done
[ -L "$cache/link.bc" ] || fail "a symlink in the cache was removed"
echo "$out" | grep -q '^Removed 2 of 6 bytecode cache entries in .*: their source files no longer exist\.$' ||
    fail "summary line: $out"
if [ $check_locked = 1 ]; then
    echo "$out" | grep -q '^Kept 3 entries whose source could not be checked\.$' || fail "kept line: $out"
    echo "$out" | grep -q "^kept .*/pascal-Locked\.pas-.*: cannot check its source .*/locked/Locked\.pas: " ||
        fail "verbose line for the unreadable source: $out"
fi
echo "$out" | grep -q "^removed .*/pascal-Gone\.pas-.*: its source .*/Gone\.pas no longer exists$" ||
    fail "verbose line for the removed entry: $out"
echo "$out" | grep -q "^kept .*/newer\.bc: it was written by a newer build$" || fail "verbose newer line: $out"
echo "$out" | grep -q "^kept .*/junk\.bc: it is not a PSB3 cache entry$" || fail "verbose junk line: $out"

# What survives is untouched: the kept entry still loads.
out=$(cd "$src" && HOME="$home" "$BIN" --verbose Keep.pas 2>&1)
echo "$out" | grep -q 'Loaded cached bytecode' || fail "the kept entry no longer loads: $out"
out=$(HOME="$home" "$BIN" --prune-cache 2>&1) || fail "second prune failed: $out"
echo "$out" | grep -q '^Removed 0 of 4 ' || fail "second prune: $out"
echo "$out" | grep -q -- '--verbose lists them' || fail "second prune does not point at --verbose: $out"

# No cache yet: nothing to do, and nothing created.
out=$(HOME="$root/fresh" "$BIN" --prune-cache 2>&1) || fail "prune without a cache failed: $out"
echo "$out" | grep -q 'nothing to prune' || fail "prune without a cache: $out"
[ ! -e "$root/fresh" ] || fail "prune created a cache directory"

# Every front end that writes the cache offers --prune-cache.
# check <name> <binary> <file> <program text>
check() {
    local name=$1 bin=$2 file=$3 text=$4 h="$root/home-$1" out
    if [ -z "$bin" ] || [ ! -x "$bin" ]; then
        echo "SKIP $name: binary not found ($bin)"
        return
    fi
    mkdir -p "$h"
    printf '%s\n' "$text" > "$src/$file"
    touch -t 202001010000 "$src/$file"
    (cd "$src" && HOME="$h" "$bin" "$file" > /dev/null 2>&1) || fail "$name: compiling $file"
    [ "$(ls "$h/.pscal/bc_cache" | wc -l | tr -d ' ')" = 1 ] || fail "$name: expected one entry"
    rm "$src/$file"
    out=$(HOME="$h" "$bin" --prune-cache 2>&1) || fail "$name --prune-cache failed: $out"
    echo "$out" | grep -q '^Removed 1 of 1 ' || fail "$name --prune-cache: $out"
    [ "$(ls "$h/.pscal/bc_cache" | wc -l | tr -d ' ')" = 0 ] || fail "$name: the entry survived"
    echo "ok $name"
}
check rea "${REA_BIN:-}" p.rea 'writeln("ok");'
check aether "${AETHER_BIN:-}" p.aether 'fn main() -> Void {
    fx { println("ok"); }
    ret;
}'
check clike "${CLIKE_BIN:-}" p.cl 'int main() { printf("ok\n"); return 0; }'
check exsh "${EXSH_BIN:-}" p.sh 'echo ok'
echo PASS
