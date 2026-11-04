#!/bin/bash
# A mutable static in a frontend's compile path is shared by every command on iOS, where a
# pipeline's stages and a terminal's tabs are threads of one process. compiler.c's made
# `clike a | clike b` crash the whole app. Such a variable has to be PSCAL_THREAD_LOCAL.
# Usage: ios_host_statics.sh <iOS-flavoured build dir>. Lists what nm finds in writable data.
set -euo pipefail
BUILD_DIR="${1:?usage: ios_host_statics.sh <build dir>}"
if [ "$(uname)" != "Darwin" ]; then
    echo "SKIP ios_host_statics: nm -m is macOS only"
    exit 0
fi

# Read-only in practice, or process-wide on purpose. Add here only with a reason.
ALLOWED='
_CLIKE_USAGE
_PASCAL_USAGE
_REA_USAGE
_holdPendingWarnings.registered
_aetherCollectionTypeAdvice.arrayNames
_aetherCollectionTypeAdvice.mapNames
_g_sigArity
_g_sigArityCount
_g_sigArityCap
_g_sigArityOnce
_break_requested
_globals_mutex
'

list_writable() {
    nm -m "$1" 2>/dev/null | awk -v want="$2" '
        /\.o:$/ { obj = $1; sub(/:$/, "", obj); next }
        /\(__DATA,__data\)|\(__DATA,__bss\)|\(__DATA,__common\)/ {
            if ($NF !~ /^ltmp/ && (want == "" || index(want, " " obj " "))) print obj, $NF
        }'
}

found=$(
    list_writable "$BUILD_DIR/libpscal_pascal_static.a" ""
    list_writable "$BUILD_DIR/libpscal_clike_static.a" ""
    list_writable "$BUILD_DIR/libpscal_rea_static.a" ""
    list_writable "$BUILD_DIR/libpscal_aether_static.a" ""
    list_writable "$BUILD_DIR/libpscal_core_static.a" " compiler.c.o globals.c.o frontend_kind.c.o diagmap.c.o scp.c.o sftp.c.o sftp-glob.c.o sftp-usergroup.c.o mac.c.o "
)

status=0
while read -r obj symbol; do
    [ -n "$symbol" ] || continue
    if ! printf '%s' "$ALLOWED" | grep -qx -- "$symbol"; then
        echo "FAIL ios_host_statics: $symbol in $obj is process-wide; make it PSCAL_THREAD_LOCAL"
        status=1
    fi
done <<< "$found"
# strtok keeps its place in libc's one static, which no nm of our data shows. Two loops starting
# together read each other's metadata through it. grep reads all of nm's output, as with pipefail
# -q could end nm early and hide a match.
if nm -u "$BUILD_DIR/bin/exsh" 2>/dev/null | grep -x _strtok > /dev/null; then
    echo "FAIL ios_host_statics: exsh calls strtok, whose place in a string is process-wide; use strtok_r"
    status=1
fi
# The same for what answers in one buffer of libc's: the macros in vproc_shim.h and pscal_ios_shim.h
# keep these from libc, so a reference means a source got past them. hcreate has one table per process.
for name in _basename _dirname _hcreate _hsearch _hdestroy; do
    if nm -u "$BUILD_DIR/bin/exsh" 2>/dev/null | grep -x "$name" > /dev/null; then
        echo "FAIL ios_host_statics: exsh calls ${name#_}, whose state is process-wide"
        status=1
    fi
done
# libc's getopt keeps its place for the whole process. An applet gets the per-thread one through thread_getopt.h.
for name in _getopt _getopt_long _optarg _opterr _optind _optopt _optreset; do
    if nm -u "$BUILD_DIR/bin/exsh" 2>/dev/null | grep -x "$name" > /dev/null; then
        echo "FAIL ios_host_statics: exsh uses libc's ${name#_}, whose state is process-wide; include thread_getopt.h"
        status=1
    fi
done

[ "$status" -eq 0 ] && echo "PASS ios_host_statics"
exit "$status"
