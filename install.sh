#!/bin/sh
set -eu

usage() {
    cat <<'USAGE'
Usage: install.sh [--prefix DIR] [--build-dir DIR]

Options:
  --prefix DIR      Installation prefix (default: /usr/local)
  --build-dir DIR   Location of built binaries relative to the repository
                    root or as an absolute path (default: build/bin)
  -h, --help        Show this help message and exit
USAGE
}

PREFIX=/usr/local
BUILD_DIR=build/bin

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix=*)
            PREFIX=${1#*=}
            ;;
        --prefix)
            shift
            [ "$#" -gt 0 ] || { echo "Error: missing value for --prefix" >&2; exit 1; }
            PREFIX=$1
            ;;
        --build-dir=*)
            BUILD_DIR=${1#*=}
            ;;
        --build-dir)
            shift
            [ "$#" -gt 0 ] || { echo "Error: missing value for --build-dir" >&2; exit 1; }
            BUILD_DIR=$1
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
PS_PREFIX=$PREFIX/pscal

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
