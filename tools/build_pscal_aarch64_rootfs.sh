#!/usr/bin/env bash
# Build a self-contained aarch64-linux-musl rootfs bundle for PSCAL, meant to
# run as a custom guest under ish-AOK's native aarch64 JIT (see
# auto-memory pscal-ish-aok-aarch64-spike.md for the full validation history:
# all 5 frontends + exsh job control confirmed working via ish-cli -r).
#
# Builds all 5 PSCAL frontends (pascal/aether/rea/clike/exsh) fully statically
# in an aarch64 Alpine Docker container (native on Apple Silicon, no
# emulation needed), stages each frontend's runtime library units and
# examples at their correct real search paths (verified against
# pscal-core/clike/rea source, not guessed -- rea/aether share ONE resolver
# whose hardcoded fallback is /usr/local/lib/rea, NOT the
# /usr/local/pscal/<lang>/lib pattern pascal/clike use), and packages the
# result as a tar.gz alongside a plain Alpine aarch64 minirootfs.
#
# This deliberately does NOT touch smallclue -- that's its own isolated
# workstream. This script uses Alpine's own busybox/coreutils for the
# non-PSCAL userland for now; swapping in a smallclue-only "zero distro"
# userland is a follow-up once smallclue's own build stabilizes.
#
# Usage: tools/build_pscal_aarch64_rootfs.sh [output_dir]
# Requires: docker (with aarch64 support -- native on Apple Silicon), curl.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PBUILD_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="${1:-$PBUILD_ROOT/build/pscal-aarch64-rootfs}"
ALPINE_VERSION="3.23.3"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v3.23/releases/aarch64/alpine-minirootfs-${ALPINE_VERSION}-aarch64.tar.gz"

FRONTENDS=(pascal aether rea clike exsh)

mkdir -p "$OUT_DIR/bin" "$OUT_DIR/rootfs" "$OUT_DIR/logs"

echo "== Step 1/4: cross-build all frontends (aarch64-linux-musl, static) =="
docker run --rm --platform linux/arm64 \
  -v "$OUT_DIR/bin:/out" \
  alpine:3.23 sh -c '
set -eu
apk add --no-cache build-base cmake git musl-dev linux-headers >/tmp/apk.log 2>&1 \
  || (tail -60 /tmp/apk.log; exit 1)
FLAGS="-static -no-pie -fno-pie"
LFLAGS="-static -no-pie"
for repo in pascal aether rea clike exsh; do
  echo "=== building $repo ==="
  if [ "$repo" = "aether" ]; then
    git clone --depth 1 --recurse-submodules "https://github.com/emkey1/$repo.git" "/work/$repo" \
      >/tmp/clone-$repo.log 2>&1 || (tail -80 /tmp/clone-$repo.log; exit 1)
  else
    git clone --depth 1 "https://github.com/emkey1/$repo.git" "/work/$repo" \
      >/tmp/clone-$repo.log 2>&1 || (tail -50 /tmp/clone-$repo.log; exit 1)
  fi
  cd "/work/$repo"
  mkdir build && cd build
  cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_EXE_LINKER_FLAGS="$LFLAGS" \
    >/tmp/cmake-$repo.log 2>&1 || (echo "CMAKE FAIL $repo"; tail -100 /tmp/cmake-$repo.log; exit 1)
  cmake --build . -j"$(nproc)" >/tmp/build-$repo.log 2>&1 \
    || (echo "BUILD FAIL $repo"; tail -150 /tmp/build-$repo.log; exit 1)
  find . -maxdepth 1 -type f -executable -name "$repo" -exec cp {} /out/$repo \;
  file /out/$repo || (echo "MISSING BINARY $repo"; exit 1)
done
echo ALL_DONE
' 2>&1 | tee "$OUT_DIR/logs/01-cross-build.log" | tail -40

for f in "${FRONTENDS[@]}"; do
  if [ ! -f "$OUT_DIR/bin/$f" ]; then
    echo "FATAL: $f binary missing after cross-build, see $OUT_DIR/logs/01-cross-build.log" >&2
    exit 1
  fi
  if ! file "$OUT_DIR/bin/$f" | grep -q "statically linked"; then
    echo "FATAL: $f is not statically linked" >&2
    file "$OUT_DIR/bin/$f" >&2
    exit 1
  fi
done
echo "All 5 frontends built and confirmed statically linked."

echo "== Step 2/4: fetch Alpine ${ALPINE_VERSION} aarch64 minirootfs =="
if [ ! -f "$OUT_DIR/alpine-minirootfs.tar.gz" ]; then
  curl -fL -o "$OUT_DIR/alpine-minirootfs.tar.gz" "$ALPINE_URL"
fi

echo "== Step 3/4: assemble rootfs =="
rm -rf "$OUT_DIR/rootfs"
mkdir -p "$OUT_DIR/rootfs"
tar xzf "$OUT_DIR/alpine-minirootfs.tar.gz" -C "$OUT_DIR/rootfs"

# Binaries: one shared root under /usr/local/pscal, matching the
# PSCAL_INSTALL_ROOT convention already compiled into pascal/clike's default
# library search path (components/pscal-core/src/core/pscal_paths.h.in).
mkdir -p "$OUT_DIR/rootfs/usr/local/pscal/bin"
for f in "${FRONTENDS[@]}"; do
  cp "$OUT_DIR/bin/$f" "$OUT_DIR/rootfs/usr/local/pscal/bin/$f"
  chmod +x "$OUT_DIR/rootfs/usr/local/pscal/bin/$f"
done
# Symlink onto Alpine's default PATH so `pascal foo.pas` works without a
# full path once a shell (exsh or busybox ash) is running -- verified this
# resolves correctly via exsh's own internal PATH search, not just via
# explicit absolute paths.
for f in "${FRONTENDS[@]}"; do
  ln -sf "/usr/local/pscal/bin/$f" "$OUT_DIR/rootfs/usr/local/bin/$f"
done

# Runtime library units, at their REAL search paths (verified against
# source via a dedicated research pass, not guessed -- pascal/clike use
# PSCAL_INSTALL_ROOT-based paths; rea/aether share one resolver whose
# hardcoded fallback is /usr/local/lib/rea, NOT /usr/local/pscal/rea/lib).
mkdir -p "$OUT_DIR/rootfs/usr/local/pscal/pascal/lib"
cp "$PBUILD_ROOT/lib/pascal"/*.pl "$OUT_DIR/rootfs/usr/local/pscal/pascal/lib/" 2>/dev/null || true

mkdir -p "$OUT_DIR/rootfs/usr/local/pscal/clike/lib"
cp "$PBUILD_ROOT/lib/clike"/*.cl "$OUT_DIR/rootfs/usr/local/pscal/clike/lib/" 2>/dev/null || true

mkdir -p "$OUT_DIR/rootfs/usr/local/lib/rea"
find "$PBUILD_ROOT/lib/rea" -maxdepth 1 -type f ! -iname "README.md" \
  -exec cp {} "$OUT_DIR/rootfs/usr/local/lib/rea/" \;

# Examples, for reference / self-hosted smoke testing inside the guest.
# Most frontends keep theirs under examples/base/; exsh uses examples/exsh/
# instead (different convention, verified by listing the actual directory).
mkdir -p "$OUT_DIR/rootfs/usr/local/pscal/examples"
for f in "${FRONTENDS[@]}"; do
  src="$PBUILD_ROOT/components/$f/examples/base"
  if [ "$f" = "exsh" ]; then
    src="$PBUILD_ROOT/components/$f/examples/exsh"
  fi
  if [ -d "$src" ]; then
    mkdir -p "$OUT_DIR/rootfs/usr/local/pscal/examples/$f"
    cp -r "$src/." "$OUT_DIR/rootfs/usr/local/pscal/examples/$f/"
  fi
done

echo "== Step 4/4: package =="
( cd "$OUT_DIR/rootfs" && tar czf "$OUT_DIR/pscal-aarch64-rootfs.tar.gz" . )
echo "Wrote $OUT_DIR/pscal-aarch64-rootfs.tar.gz"
du -sh "$OUT_DIR/pscal-aarch64-rootfs.tar.gz"

cat <<'EOF'

To validate: build ish-AOK's `ish-cli` scheme (macOS/arm64 host target, see
auto-memory pscal-ish-aok-aarch64-spike.md for the exact xcodebuild recipe and
known build-config gotchas), then e.g.:

  ish-AOK -r <output_dir>/rootfs -d /usr/local/pscal/examples/pascal \
    /usr/local/pscal/bin/pascal hello

  ish-AOK -r <output_dir>/rootfs /usr/local/pscal/bin/exsh -c \
    'pascal /usr/local/pscal/examples/pascal/hello'
EOF
