#!/bin/bash
# After cd, the iOS-flavoured cache looks for a source, its imports and its entry from the vproc's
# directory. It used the process's, so a same-named program there ran instead, with no error. The
# cache only loads when it can find the frontend's binary to compare with: a run started by a bare
# name needs one on PATH, so each name here has an old empty file, and the shell still runs its own.
# Rea and Aether skip the cache on iOS unless REA_CACHE is set.
#   test_imports_cache.sh <exsh>
set -u
EXE=${1:?usage: test_imports_cache.sh <exsh>}
BASE=$(mktemp -d "${TMPDIR:-/tmp}/exsh-imports-cache.XXXXXX") || exit 1
case "$BASE" in
    */exsh-imports-cache.*) ;;
    *) echo "bad scratch directory: $BASE"; exit 1 ;;
esac
trap 'rm -rf "$BASE"' EXIT
T=$(cd "$BASE" && pwd -P)
mkdir -p "$T/bin" "$T/start"
for f in clike rea aether pascal exsh; do
    : > "$T/bin/$f"
    chmod +x "$T/bin/$f"
    touch -t 202001010000 "$T/bin/$f"
done
cat > "$T/t.sh" <<'SCRIPT'
export REA_CACHE=1
programs() {
  printf 'int main() { printf("clike %s\\n"); return 0; }\n' "$1" > r.cl
  printf 'writeln("rea %s");\n' "$1" > r.rea
  printf 'fn main() -> Void { fx { println("aether %s"); } ret; }\n' "$1" > r.aether
  printf 'program R;\nbegin\n  writeln(%spascal %s%s);\nend.\n' "'" "$1" "'" > r.pas
  printf 'echo exsh %s\n' "$1" > r.sh
}
runall() {
  clike r.cl; rea r.rea; aether r.aether; pascal r.pas; exsh r.sh
}
loads() {
  for f in 'clike r.cl' 'rea r.rea' 'aether r.aether' 'pascal r.pas' 'exsh r.sh'; do
    set -- $f
    "$1" --verbose "$2" 2>&1 | grep -c 'Loaded cached'
  done
}
imports() {
  printf 'str who() { return "clike import %s"; }\n' "$1" > imx.cl
  printf 'module Imx { export str who() { return "rea import %s"; } }\n' "$1" > imx.rea
  printf 'mod Imx { export fn who() -> Text { ret "aether import %s"; } }\n' "$1" > imx.aether
}
runimports() {
  clike "$1p.cl"; rea "$1p.rea"; aether "$1p.aether"
}
mkdir d e
programs start
echo "- start"; runall
sleep 2
echo "- start, from its entry"; loads; runall
cd d || exit 1
programs d
sleep 2
echo "- d"; runall
sleep 2
echo "- d, from its entry"; loads; runall
cd ..
echo "- start again"; runall

imports start
cd d || exit 1
imports d
cd ../e || exit 1
imports e
cd ..
printf 'import "imx.cl";\nint main() { printf("%%s\\n", who()); return 0; }\n' > p.cl
printf '#import "imx.rea";\nwriteln(who());\n' > p.rea
printf 'use "imx.aether";\nfn main() -> Void { fx { println(who()); } ret; }\n' > p.aether
sleep 2
cd d || exit 1
echo "- the same program from d"; runimports ../
sleep 2
cd ../e || exit 1
echo "- the same program from e"; runimports ../
sleep 2
echo "- from e, an import edited"
imports e2
sleep 2
runimports ../
SCRIPT
out=$(cd "$T/start" && PATH="$T/bin:$PATH" PATH_TRUNCATE="$T" HOME="$T" TMPDIR="$T" /usr/bin/perl -e 'alarm 120; exec @ARGV' "$EXE" "$T/t.sh" 2>&1 < /dev/null)
rc=$?
want="- start
clike start
rea start
aether start
pascal start
exsh start
- start, from its entry
1
1
1
1
1
clike start
rea start
aether start
pascal start
exsh start
- d
clike d
rea d
aether d
pascal d
exsh d
- d, from its entry
1
1
1
1
1
clike d
rea d
aether d
pascal d
exsh d
- start again
clike start
rea start
aether start
pascal start
exsh start
- the same program from d
clike import d
rea import d
aether import d
- the same program from e
clike import e
rea import e
aether import e
- from e, an import edited
clike import e2
rea import e2
aether import e2"
if [ "$rc" != 0 ] || [ "$out" != "$want" ]; then
    echo "FAIL imports_cache: status $rc"
    printf 'want:\n%s\ngot:\n%s\n' "$want" "$out"
    exit 1
fi
echo "ok imports_cache"
