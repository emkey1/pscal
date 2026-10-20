#!/bin/bash
# findUnitFile's "searched paths" message for a search directory longer than
# PATH_MAX used to copy snprintf's untruncated length out of its buffer (an
# ASan stack-buffer-overflow). It must report the missing unit and exit 1.
# usage: PASCAL_BIN=... unit_search_long_lib_dir.sh   (exit 0 = pass)
set -u
BIN=${PASCAL_BIN:?set PASCAL_BIN}
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
printf 'program U;\nuses nosuchunit;\nbegin\nend.\n' > "$dir/U"
long=/tmp/$(python3 -c "print('d'*250 + ('/' + 'e'*250)*20)")
out=$(cd "$dir" && PASCAL_LIB_DIR=$long HOME=$dir "$BIN" --no-cache U 2>&1); rc=$?
[ $rc -eq 1 ] || { echo "FAIL: exit $rc"; echo "$out" | head -3; exit 1; }
echo "$out" | grep -q "unit 'nosuchunit' not found" || { echo "FAIL: $out" | head -3; exit 1; }
echo PASS
