#!/bin/bash
# An import is looked for from the working directory too, so one source can compile to two
# programs from two directories. An entry is not reused for the other directory's copy, however
# old that copy is, and a module's own imports count as well.
# usage: CLIKE_BIN=... REA_BIN=... AETHER_BIN=... imports_two_directories.sh   (exit 0 = pass)
set -u
CLIKE=${CLIKE_BIN:?set CLIKE_BIN}
REA=${REA_BIN:?set REA_BIN}
AETHER=${AETHER_BIN:?set AETHER_BIN}
home=$(mktemp -d); src=$(mktemp -d)
trap 'rm -rf "$home" "$src"' EXIT
fail() { echo "FAIL: $*"; exit 1; }
# Older than any entry written here, so only what an entry records can tell the copies apart.
old() { touch -t 202001010000 "$@"; }
run() { (cd "$src/$2" && HOME="$home" "$1" --verbose "$3" 2>"$home/err"); }

mkdir -p "$src/a" "$src/b"
for d in a b; do
    printf 'str who() { return "clike %s"; }\n' "$d" > "$src/$d/imx.cl"
    printf 'module Imx { export str who() { return "rea %s"; } }\n' "$d" > "$src/$d/imx.rea"
    printf 'mod Imx { export fn who() -> Text { ret "aether %s"; } }\n' "$d" > "$src/$d/imx.aether"
    old "$src/$d/imx.cl" "$src/$d/imx.rea" "$src/$d/imx.aether"
done
printf 'import "imx.cl";\nint main() { printf("%%s\\n", who()); return 0; }\n' > "$src/p.cl"
printf '#import "imx.rea";\nwriteln(who());\n' > "$src/p.rea"
printf 'use "imx.aether";\nfn main() -> Void { fx { println(who()); } ret; }\n' > "$src/p.aether"

two_directories() {  # binary, program, what it prints
    local out
    out=$(run "$1" a "../$2"); [ "$out" = "$3 a" ] || fail "$3 from a printed '$out'"
    sleep 1
    out=$(run "$1" b "../$2"); [ "$out" = "$3 b" ] || fail "$3 from b printed '$out': an entry for a's import was reused"
    sleep 1
    out=$(run "$1" b "../$2"); [ "$out" = "$3 b" ] || fail "$3 again from b printed '$out'"
    grep -q 'Loaded cached bytecode' "$home/err" || fail "$3 did not load its entry: $(cat "$home/err")"
    out=$(run "$1" a "../$2"); [ "$out" = "$3 a" ] || fail "$3 back in a printed '$out'"
}
two_directories "$CLIKE" p.cl clike
two_directories "$REA" p.rea rea
two_directories "$AETHER" p.aether aether

# What a module imports. Not CLike: an imported file's own import does not work there on any host.
printf '#import "imn.rea";\nmodule Imo { export str who() { return leaf(); } }\n' > "$src/a/imo.rea"
printf 'module Imn { export str leaf() { return "rea leaf 1"; } }\n' > "$src/a/imn.rea"
printf '#import "imo.rea";\nwriteln(who());\n' > "$src/a/q.rea"
printf 'use "imn.aether";\nmod Imo { export fn who() -> Text { ret leaf(); } }\n' > "$src/a/imo.aether"
printf 'mod Imn { export fn leaf() -> Text { ret "aether leaf 1"; } }\n' > "$src/a/imn.aether"
printf 'use "imo.aether";\nfn main() -> Void { fx { println(who()); } ret; }\n' > "$src/a/q.aether"
sleep 1
nested() {  # binary, program, inner file, what it prints, the inner file edited
    local out
    out=$(run "$1" a "$2"); [ "$out" = "$4 leaf 1" ] || fail "$4 nested printed '$out'"
    sleep 1
    out=$(run "$1" a "$2"); [ "$out" = "$4 leaf 1" ] || fail "$4 nested again printed '$out'"
    grep -q 'Loaded cached bytecode' "$home/err" || fail "$4 nested did not load its entry: $(cat "$home/err")"
    printf '%s\n' "$5" > "$src/a/$3"
    old "$src/a/$3"
    out=$(run "$1" a "$2"); [ "$out" = "$4 leaf 2" ] || fail "$4 nested after an edit printed '$out'"
}
nested "$REA" q.rea imn.rea rea 'module Imn { export str leaf() { return "rea leaf 2"; } }'
nested "$AETHER" q.aether imn.aether aether 'mod Imn { export fn leaf() -> Text { ret "aether leaf 2"; } }'
echo PASS
