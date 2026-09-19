#!/usr/bin/env bash
# Host-side driver for tools/build_pscal_pure_glibc_rootfs.sh.
#
# That script is written to run INSIDE an aarch64 Debian container: it
# apt-installs its own toolchain, clones each component from GitHub, and
# assembles the rootfs as uid 0. It used to be invoked by an ad-hoc `docker
# run` line that lived nowhere but a shell history, which is why nobody could
# rebuild the published image without reverse-engineering the bind mounts from
# the script's own hardcoded paths. This is that line, kept.
#
# Usage: tools/build_pscal_rootfs_docker.sh [output_dir]
#   PSCAL_ROOTFS_VERSION=2026.09.19  version stamped into the archive name,
#                                    /etc/pscal-release and /etc/os-release
#                                    (default: today's UTC date)
#   SMALLCLUE_PIN=<sha>              smallclue commit to build (default: the
#                                    pin in the in-container script)
#   BASE_IMAGE=debian:bookworm-slim  bookworm, deliberately: trixie ships
#                                    OpenSSL 3.5, whose headers smallclue's
#                                    vendored OpenSSH configure rejects
#                                    outright ("working libcrypto not found").
#
# Requires: Docker with linux/arm64 support -- native on Apple Silicon, which
# is the only configuration this has been run on.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PBUILD_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="${1:-$PBUILD_ROOT/build/pscal-rootfs}"
BASE_IMAGE="${BASE_IMAGE:-debian:bookworm-slim}"
PSCAL_ROOTFS_VERSION="${PSCAL_ROOTFS_VERSION:-$(date -u +%Y.%m.%d)}"

if ! docker info >/dev/null 2>&1; then
    echo "ERROR: Docker daemon is not reachable. Start Docker Desktop and retry." >&2
    exit 1
fi

# Docker Desktop on macOS only shares a fixed set of host paths, and /tmp and
# /var/folders are not among them. A bind mount of an unshared path does not
# fail -- it silently comes up as an empty directory inside the container, so
# the build would run to completion and the finished archive would land
# nowhere. Refuse that up front instead of at the end of an hour.
case "$OUT_DIR" in
    /Users/*|/Volumes/*) ;;
    *)
        echo "ERROR: $OUT_DIR is outside the paths Docker Desktop shares." >&2
        echo "       Pick somewhere under /Users (the default is $PBUILD_ROOT/build/pscal-rootfs)." >&2
        exit 1
        ;;
esac

mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

echo "== PSCAL rootfs $PSCAL_ROOTFS_VERSION =="
echo "   base image: $BASE_IMAGE (linux/arm64)"
echo "   output:     $OUT_DIR"

# The frontends' runtime library units (pascal/*.pl, clike/*.cl, rea/*) are
# staged from this working tree, not re-cloned, so a local change to a library
# unit can be tested in a guest without pushing it first. Read-only: the
# container must never be able to write back into the checkout.
docker run --rm --platform linux/arm64 \
    -e "PSCAL_ROOTFS_VERSION=$PSCAL_ROOTFS_VERSION" \
    ${SMALLCLUE_PIN:+-e "SMALLCLUE_PIN=$SMALLCLUE_PIN"} \
    -v "$OUT_DIR:/out" \
    -v "$PBUILD_ROOT/lib:/pbuild-lib:ro" \
    -v "$PBUILD_ROOT/third-party/curl-8.17.0:/pbuild-curl:ro" \
    -v "$SCRIPT_DIR/build_pscal_pure_glibc_rootfs.sh:/build.sh:ro" \
    "$BASE_IMAGE" \
    bash /build.sh /out

echo
echo "Built: $OUT_DIR/pscal-rootfs-${PSCAL_ROOTFS_VERSION}-aarch64.tar.xz"
