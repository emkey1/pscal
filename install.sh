#!/bin/sh
set -eu

usage() {
    cat <<'USAGE'
Usage: install.sh [--prefix DIR] [--pscal-dir DIR] [--build-dir DIR]

Options:
  --prefix DIR      Installation prefix (default: value from the build cache
                    or /usr/local)
  --pscal-dir DIR   Destination for PSCAL libraries and assets.
                    Defaults to the compiled-in install root when available
                    (unless --prefix is provided), otherwise
                    "$PREFIX/pscal". Relative paths are resolved against
                    --prefix.
  --build-dir DIR   Location of built binaries relative to the repository
                    root or as an absolute path (default: build/bin)
  -h, --help        Show this help message and exit
USAGE
}

PREFIX=/usr/local
PREFIX_SET=0
BUILD_DIR=build/bin
PSCAL_DIR_OVERRIDE=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix=*)
            PREFIX=${1#*=}
            PREFIX_SET=1
            ;;
        --prefix)
            shift
            [ "$#" -gt 0 ] || { echo "Error: missing value for --prefix" >&2; exit 1; }
            PREFIX=$1
            PREFIX_SET=1
            ;;
        --build-dir=*)
            BUILD_DIR=${1#*=}
            ;;
        --build-dir)
            shift
            [ "$#" -gt 0 ] || { echo "Error: missing value for --build-dir" >&2; exit 1; }
            BUILD_DIR=$1
            ;;
        --pscal-dir=*)
            PSCAL_DIR_OVERRIDE=${1#*=}
            ;;
        --pscal-dir)
            shift
            [ "$#" -gt 0 ] || { echo "Error: missing value for --pscal-dir" >&2; exit 1; }
            PSCAL_DIR_OVERRIDE=$1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown option $1" >&2
            usage >&2
            exit 1
            ;;
    esac
    shift
done

PREFIX=${PREFIX%/}
[ -n "$PREFIX" ] || PREFIX="/"

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$SCRIPT_DIR

case $BUILD_DIR in
    /*)
        BIN_SRC=$BUILD_DIR
        ;;
    *)
        BIN_SRC=$REPO_ROOT/$BUILD_DIR
        ;;
esac

BUILD_ROOT=$(dirname "$BIN_SRC")

if [ "$PREFIX_SET" -eq 0 ]; then
    CACHE_FILE=$BUILD_ROOT/CMakeCache.txt
    if [ -f "$CACHE_FILE" ]; then
        CACHE_PREFIX=$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$CACHE_FILE" | tail -n 1)
        if [ -n "$CACHE_PREFIX" ]; then
            PREFIX=$CACHE_PREFIX
        fi
    fi
fi

resolve_pscal_dir() {
    candidate=$1
    if [ -z "$candidate" ]; then
        return
    fi
    case $candidate in
        /*)
            printf '%s' "${candidate%/}"
            ;;
        *)
            printf '%s' "${PREFIX%/}/$candidate"
            ;;
    esac
}

PS_PREFIX=""

if [ -n "$PSCAL_DIR_OVERRIDE" ]; then
    PS_PREFIX=$(resolve_pscal_dir "$PSCAL_DIR_OVERRIDE")
fi

if [ -z "$PS_PREFIX" ] && [ -n "${PSCAL_INSTALL_ROOT:-}" ]; then
    PS_PREFIX=$(resolve_pscal_dir "$PSCAL_INSTALL_ROOT")
fi

if [ -z "$PS_PREFIX" ] && [ "$PREFIX_SET" -eq 0 ] && [ -f "$BUILD_ROOT/pscal_install_root.txt" ]; then
    if read -r line <"$BUILD_ROOT/pscal_install_root.txt"; then
        PS_PREFIX=$(resolve_pscal_dir "$line")
    fi
fi

if [ -z "$PS_PREFIX" ]; then
    PS_PREFIX=${PREFIX%/}/pscal
fi

PS_PREFIX=${PS_PREFIX%/}

if ! mkdir -p "$PREFIX" 2>/dev/null; then
    echo "Error: unable to create or access $PREFIX" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    TEST_DIR=$PREFIX/.pscal-install-test.$$ 
    if ! mkdir -p "$TEST_DIR" 2>/dev/null; then
        echo "Error: insufficient permissions to write to $PREFIX" >&2
        exit 1
    fi
    rmdir "$TEST_DIR"
fi

BINDIR=$PREFIX/bin

if ! mkdir -p "$PS_PREFIX" 2>/dev/null; then
    echo "Error: unable to create or access $PS_PREFIX" >&2
    exit 1
fi

if [ "$(id -u)" -ne 0 ]; then
    TEST_DIR=$PS_PREFIX/.pscal-install-test.$$
    if ! mkdir -p "$TEST_DIR" 2>/dev/null; then
        echo "Error: insufficient permissions to write to $PS_PREFIX" >&2
        exit 1
    fi
    rmdir "$TEST_DIR"
fi

copy_dir_contents() {
    src=$1
    dest=$2
    description=$3

    if [ -d "$src" ]; then
        mkdir -p "$dest"
        if [ -n "$(find "$src" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
            cp -R "$src"/. "$dest"/
        fi
    else
        echo "Notice: skipping $description (not found: $src)" >&2
    fi
}

# Install binaries if available
if [ -d "$BIN_SRC" ]; then
    mkdir -p "$BINDIR"
    cp -R "$BIN_SRC"/. "$BINDIR"/
else
    echo "Notice: skipping binary installation (not found: $BIN_SRC)" >&2
fi

# Shared libraries and assets
copy_dir_contents "$REPO_ROOT/lib" "$PS_PREFIX/lib" "library files"
copy_dir_contents "$REPO_ROOT/lib/rea" "$PREFIX/lib/rea" "Rea import library"
copy_dir_contents "$REPO_ROOT/fonts" "$PS_PREFIX/fonts" "fonts"
copy_dir_contents "$REPO_ROOT/etc" "$PS_PREFIX/etc" "configuration files"
copy_dir_contents "$REPO_ROOT/Tests/libs" "$PS_PREFIX/etc/tests" "test configuration files"
copy_dir_contents "$REPO_ROOT/Docs" "$PS_PREFIX/docs" "Doc files"

# Backwards compatibility shims for legacy layouts
create_compat_link() {
    target=$1
    link=$2

    if [ ! -e "$target" ] && [ ! -L "$target" ]; then
        return
    fi

    if [ -e "$link" ] || [ -L "$link" ]; then
        return
    fi

    parent_dir=$(dirname "$link")
    mkdir -p "$parent_dir"
    ln -s "$target" "$link"
}

create_compat_link "$PS_PREFIX/lib/pascal" "$PS_PREFIX/pascal/lib"
create_compat_link "$PS_PREFIX/lib/clike" "$PS_PREFIX/clike/lib"
create_compat_link "$PS_PREFIX/lib/rea" "$PS_PREFIX/rea/lib"
create_compat_link "$PS_PREFIX/lib/misc" "$PS_PREFIX/misc"

# Ensure expected binaries are executable when installed
for bin in clike clike-repl dascal pascal pscalvm rea pscaljson2bc pscald; do
    candidate=$BINDIR/$bin
    if [ -f "$candidate" ]; then
        chmod go+x "$candidate"
    fi
done

exit 0
