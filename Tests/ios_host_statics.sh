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
_interrupted
_alarm_fired
_win_resized
_client_x11_get_proto.data
_client_x11_get_proto.proto
_esc_txt
_compat_banner.check
_proposals_match.check
_pscalKexBannerRetryBudgetMs.cached
_dh_new_group1.gen
_dh_new_group1.group1
_dh_new_group14.gen
_dh_new_group14.group14
_dh_new_group16.gen
_dh_new_group16.group16
_dh_new_group18.gen
_dh_new_group18.group18
_moduli_filename
_crypto_int16_optblocker
_crypto_int32_optblocker
_crypto_int64_optblocker
_monotime_ts.gettime_failed
_default_ssh_port.port
_kex_default_pk_alg.pkalgs
_keywords
_log_facilities
_log_levels
_ssh_bzero
_cclasses
_signo
_pick_salt.salt
_helpers
_nhelpers
_ssh_init.called
_umac_ctx
_umac128_ctx
_g_pscal_ios_tty_lock
_g_pscal_ios_virtual_ttys
_pscal_ios_translate_etc_path.kPrefixes
_pscal_openssh_hostkey_dir.dirbuf
_pscal_openssh_hostkey_path.pathbuf
'

list_writable() {
    nm -m "$1" 2>/dev/null | awk -v want="$2" '
        /\.o:$/ { obj = $1; sub(/:$/, "", obj); next }
        /\(__DATA,__data\)|\(__DATA,__bss\)|\(__DATA,__common\)|\(common\)/ {
            if ($NF !~ /^ltmp/ && (want == "" || index(want, " " obj " "))) print obj, $NF
        }'
}

# ssh, scp and sftp can run at once in the app as well, on threads of their own, so what one
# run keeps is PSCAL_SSH_THREAD_LOCAL. These go by file: openrsync has a log.c and a misc.c too,
# and the archive names both alike. ssh-keygen runs alone, and moduli is only part of it.
list_writable_openssh() {
    local dir="$BUILD_DIR/CMakeFiles/pscal_core_static.dir/third-party/openssh-10.6p2"
    [ -d "$dir" ] || return 0
    find "$dir" -name '*.c.o' ! -name 'ssh-keygen.c.o' ! -name 'moduli.c.o' | sort | while read -r object; do
        nm -m "$object" 2>/dev/null | awk -v obj="$(basename "$object")" '
            /\(__DATA,__data\)|\(__DATA,__bss\)|\(__DATA,__common\)|\(common\)/ {
                if ($NF !~ /^ltmp/) print obj, $NF
            }'
    done
}

found=$(
    list_writable "$BUILD_DIR/libpscal_pascal_static.a" ""
    list_writable "$BUILD_DIR/libpscal_clike_static.a" ""
    list_writable "$BUILD_DIR/libpscal_rea_static.a" ""
    list_writable "$BUILD_DIR/libpscal_aether_static.a" ""
    list_writable "$BUILD_DIR/libpscal_core_static.a" " compiler.c.o globals.c.o frontend_kind.c.o diagmap.c.o "
    list_writable_openssh
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

# setlocale is the whole app's, and libc crashes if two threads load a locale at once.
if nm -u "$BUILD_DIR/bin/exsh" 2>/dev/null | grep -x _setlocale > /dev/null; then
    echo "FAIL ios_host_statics: exsh calls setlocale, which is the whole app's; the macros in vproc_shim.h and pscal_ios_shim.h keep it from libc"
    status=1
fi

# libc's environment is the whole app's, and a capture of it for a command crashed when a client's
# unsetenv shifted it. Through pscal_ios_shim.h a client reads and changes the one it was given.
openssh_objects="$BUILD_DIR/CMakeFiles/pscal_core_static.dir/third-party/openssh-10.6p2"
if [ -d "$openssh_objects" ]; then
    while read -r object; do
        if nm -u "$object" 2>/dev/null | grep -xE '_(getenv|setenv|unsetenv|putenv|environ)' > /dev/null; then
            echo "FAIL ios_host_statics: $(basename "$object") uses libc's environment, which is the whole app's"
            status=1
        fi
    done < <(find "$openssh_objects" -name '*.c.o' | sort)
fi

[ "$status" -eq 0 ] && echo "PASS ios_host_statics"
exit "$status"
