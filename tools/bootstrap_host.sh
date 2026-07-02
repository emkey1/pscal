#!/usr/bin/env bash
# Bootstrap a fresh (usually Debian/Devuan) host for PSCAL umbrella builds.
#
# Turns the serial whack-a-mole (wedged submodules -> missing dep -> missing
# dep -> ...) into one pass: clone or repair, verify submodule health, install
# every known build dependency up front, then configure and report ALL
# problems at once.
#
# Usage:
#   tools/bootstrap_host.sh [--dir DIR] [--sdl] [--no-clone] [--jobs N]
#
#   --dir DIR    checkout location (default: ~/PBuild)
#   --sdl        install SDL2 deps and configure with -DSDL=ON
#   --no-clone   skip clone; repair/verify an existing checkout in --dir
#   --jobs N     parallel build jobs for the smoke build (default: nproc)
#
# Notes:
#  - Uses the SSH remote; the https submodule URLs fail on credential-less
#    hosts ("could not read Username ... Device not configured").
#  - Interrupted recursive clones wedge submodules; this script detects and
#    repairs the known failure modes (stale index.lock, .invalid HEAD, empty
#    index) instead of leaving you to rediscover them.
#  - Deliberately `set -u` (not -e): collect and report every failure, only
#    exit nonzero at the end.

set -u

REPO_SSH="git@github.com:emkey1/pscal.git"
DIR="$HOME/PBuild"
WANT_SDL=0
DO_CLONE=1
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

while [ $# -gt 0 ]; do
  case "$1" in
    --dir) DIR="$2"; shift 2 ;;
    --sdl) WANT_SDL=1; shift ;;
    --no-clone) DO_CLONE=0; shift ;;
    --jobs) JOBS="$2"; shift 2 ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done

FAILURES=()
note() { printf '== %s\n' "$*"; }
fail() { FAILURES+=("$*"); printf 'XX %s\n' "$*" >&2; }

# ---------------------------------------------------------------- deps ------
if command -v apt-get >/dev/null 2>&1; then
  note "installing build dependencies (apt)"
  DEPS=(build-essential cmake git pkg-config
        libcurl4-openssl-dev libssl-dev zlib1g-dev libsqlite3-dev)
  [ "$WANT_SDL" = 1 ] && DEPS+=(libsdl2-dev libsdl2-ttf-dev libsdl2-mixer-dev)
  sudo apt-get update -qq || fail "apt-get update failed"
  sudo apt-get install -y "${DEPS[@]}" || fail "apt-get install failed (${DEPS[*]})"
else
  note "no apt-get here; assuming deps are present (macOS/other)"
fi

# ---------------------------------------------------------------- clone -----
if [ "$DO_CLONE" = 1 ] && [ ! -d "$DIR/.git" ]; then
  note "cloning $REPO_SSH -> $DIR"
  git clone --recurse-submodules --no-recommend-shallow "$REPO_SSH" "$DIR" \
    || fail "clone failed (network blip? rerun with --no-clone to repair)"
fi
cd "$DIR" || { fail "checkout dir $DIR missing"; printf 'aborting\n'; exit 1; }

# ------------------------------------------------- submodule health ---------
note "verifying/repairing submodules"
# Known wedge repairs from interrupted recursive clones:
find .git/modules -name index.lock -mmin +10 -print -delete 2>/dev/null
git submodule foreach --recursive --quiet '
  headref=$(git symbolic-ref -q HEAD || true)
  if [ -n "$headref" ] && ! git rev-parse -q --verify HEAD >/dev/null 2>&1; then
    echo "  repairing wedged HEAD in $name"
    git checkout --force "$(git -C "$toplevel" ls-tree -d HEAD "$sm_path" | awk "{print \$3}")" 2>/dev/null || true
  fi
' 2>/dev/null

git submodule update --init --recursive --no-recommend-shallow \
  || fail "submodule update failed; inspect 'git submodule status --recursive'"

BAD=$(git submodule status --recursive | grep -c '^[-U+]' || true)
if [ "${BAD:-0}" -gt 0 ]; then
  git submodule status --recursive | grep '^[-U+]' | sed 's/^/  /'
  fail "$BAD submodule(s) not cleanly checked out (see above)"
fi

# ---------------------------------------------------------------- cmake -----
note "configuring (all missing-dep errors should surface in this one pass)"
CMAKE_ARGS=(-S . -B build)
[ "$WANT_SDL" = 1 ] && CMAKE_ARGS+=(-DSDL=ON)
cmake "${CMAKE_ARGS[@]}" || fail "cmake configure failed"

if [ ${#FAILURES[@]} -eq 0 ]; then
  note "smoke build: pscal core targets ($JOBS jobs)"
  cmake --build build -j "$JOBS" || fail "build failed"
fi

# ---------------------------------------------------------------- report ----
echo
if [ ${#FAILURES[@]} -eq 0 ]; then
  note "bootstrap complete: $DIR (binaries in build/bin/)"
  exit 0
fi
echo "bootstrap finished with ${#FAILURES[@]} problem(s):"
for f in "${FAILURES[@]}"; do echo "  - $f"; done
exit 1
