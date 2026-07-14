#!/usr/bin/env bash
# Pure PSCAL/SmallCLUE aarch64 rootfs for iSH-AOK -- glibc, native (no cross-compile,
# no musl). Runs natively inside a Debian aarch64 container on Apple Silicon, using
# apt-supplied -dev packages and each vendored dependency's OWN build system (real
# ./configure for OpenSSH, real CMake for libgit2) -- the same pattern the iOS build
# and the earlier i386 iSH port both use, just with a target glibc actually knows.
set -euo pipefail
OUT_DIR="${1:-/out}"
mkdir -p "$OUT_DIR/bin" "$OUT_DIR/rootfs" "$OUT_DIR/logs"

echo "== Step 1/3: build 5 PSCAL frontends + SmallCLUE natively (aarch64 glibc, static) =="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/tmp/apt.log 2>&1
apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config curl wget python3 patch openssh-client \
    zlib1g-dev libssl-dev libncurses-dev autoconf automake libtool \
    >>/tmp/apt.log 2>&1 || (tail -100 /tmp/apt.log; exit 1)
echo APT_OK

FRONTENDS="pascal aether rea clike exsh"
for repo in $FRONTENDS; do
  echo "=== building $repo ==="
  if [ "$repo" = "aether" ]; then
    git clone --depth 1 --recurse-submodules "https://github.com/emkey1/$repo.git" "/work/$repo" \
      >/tmp/clone-$repo.log 2>&1 || (tail -80 /tmp/clone-$repo.log; exit 1)
  else
    git clone --depth 1 "https://github.com/emkey1/$repo.git" "/work/$repo" \
      >/tmp/clone-$repo.log 2>&1 || (tail -50 /tmp/clone-$repo.log; exit 1)
  fi
  ( cd "/work/$repo" && mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-static" -DCMAKE_EXE_LINKER_FLAGS="-static" \
      >/tmp/cmake-$repo.log 2>&1 || (echo "CMAKE FAIL $repo"; tail -100 /tmp/cmake-$repo.log; exit 1) )
  ( cd "/work/$repo/build" && cmake --build . -j"$(nproc)" >/tmp/build-$repo.log 2>&1 \
      || (echo "BUILD FAIL $repo"; tail -150 /tmp/build-$repo.log; exit 1) )
  find "/work/$repo/build" -maxdepth 1 -type f -executable -name "$repo" -exec cp {} "$OUT_DIR/bin/$repo" \;
  [ -f "$OUT_DIR/bin/$repo" ] || { echo "MISSING BINARY $repo"; exit 1; }
done

echo "=== building smallclue (native aarch64 glibc via setup_posix_env.sh) ==="
git clone --branch main --depth 1 https://github.com/emkey1/smallclue.git /work/smallclue \
  >/tmp/clone-smallclue.log 2>&1 || (tail -80 /tmp/clone-smallclue.log; exit 1)
( cd /work/smallclue && AUTO_INSTALL_DEPS=1 bash setup_posix_env.sh >/tmp/setup-posix.log 2>&1 \
    || (echo "SETUP_POSIX_ENV FAIL"; tail -200 /tmp/setup-posix.log; exit 1) )
cp /work/smallclue/smallclue "$OUT_DIR/bin/smallclue"

for f in $FRONTENDS smallclue; do
  file "$OUT_DIR/bin/$f" | grep -q "statically linked" || { echo "FATAL: $f not statically linked"; file "$OUT_DIR/bin/$f"; exit 1; }
  file "$OUT_DIR/bin/$f" | grep -qi "aarch64" || { echo "FATAL: $f not aarch64"; file "$OUT_DIR/bin/$f"; exit 1; }
done
echo "All 6 binaries built, statically linked, aarch64 glibc."

echo "== Step 2/3: assemble pure PSCAL/SmallCLUE rootfs (merged-usr, no Alpine, no login wrapper) =="
RFS="$OUT_DIR/rootfs"
rm -rf "$RFS"
mkdir -p "$RFS/usr/bin" "$RFS/usr/local/pscal/bin" "$RFS/usr/local/pscal/pascal/lib" \
         "$RFS/usr/local/pscal/clike/lib" "$RFS/usr/local/lib/rea" "$RFS/etc" \
         "$RFS/tmp" "$RFS/var" "$RFS/home/username" "$RFS/dev/shm" "$RFS/dev/pts" \
         "$RFS/proc" "$RFS/sys" "$RFS/root"
chmod 1777 "$RFS/tmp"
ln -s usr/bin "$RFS/bin"
ln -s usr/bin "$RFS/sbin"
ln -s bin "$RFS/usr/sbin"

if mknod -m 666 "$RFS/dev/test_null" c 1 3 2>/dev/null; then
  rm -f "$RFS/dev/test_null"
  mknod -m 666 "$RFS/dev/null" c 1 3
  mknod -m 666 "$RFS/dev/zero" c 1 5
  mknod -m 666 "$RFS/dev/random" c 1 8
  mknod -m 666 "$RFS/dev/urandom" c 1 9
  mknod -m 666 "$RFS/dev/tty" c 5 0
  mknod -m 622 "$RFS/dev/console" c 5 1
  mknod -m 666 "$RFS/dev/ptmx" c 5 2
else
  echo "Warning: mknod not permitted, skipping (iSH populates /dev at runtime)."
fi

cp "$OUT_DIR/bin/smallclue" "$RFS/usr/bin/smallclue"
chmod +x "$RFS/usr/bin/smallclue"
APPLETS=$(awk '
    /static const SmallclueApplet kSmallclueApplets\[\] = \{/ { in_table = 1; next }
    in_table && /^\};/ { exit }
    in_table && match($0, /^[[:space:]]*\{"[^"]+"/) {
        line = $0
        sub(/^[[:space:]]*\{"/, "", line)
        sub(/".*$/, "", line)
        print line
    }
' /work/smallclue/src/core.c | sort -u)
for applet in $APPLETS; do
  [ "$applet" = "smallclue" ] && continue
  [ "$applet" = "exsh" ] && continue
  ln -sf smallclue "$RFS/usr/bin/$applet"
done
[ -e "$RFS/usr/bin/sh" ] || { echo "Error: no sh applet"; exit 1; }
[ -e "$RFS/usr/bin/init" ] || { echo "Error: no init applet"; exit 1; }

for f in $FRONTENDS; do
  cp "$OUT_DIR/bin/$f" "$RFS/usr/local/pscal/bin/$f"
  chmod +x "$RFS/usr/local/pscal/bin/$f"
  ln -sf /usr/local/pscal/bin/$f "$RFS/usr/bin/$f"
done

cp /pbuild-lib/pascal/*.pl "$RFS/usr/local/pscal/pascal/lib/" 2>/dev/null || true
cp /pbuild-lib/clike/*.cl "$RFS/usr/local/pscal/clike/lib/" 2>/dev/null || true
find /pbuild-lib/rea -maxdepth 1 -type f ! -iname "README.md" -exec cp {} "$RFS/usr/local/lib/rea/" \; 2>/dev/null || true

cat > "$RFS/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/usr/bin/exsh
username:x:1000:1000:User Name,,,:/home/username:/usr/bin/exsh
EOF
cat > "$RFS/etc/group" <<'EOF'
root:x:0:
username:x:1000:
EOF
cat > "$RFS/etc/hosts" <<'EOF'
127.0.0.1   localhost
::1         localhost ip6-localhost ip6-loopback
EOF
cat > "$RFS/etc/profile" <<'EOF'
export PATH=/usr/bin
export PSCAL_INSTALL_ROOT=/usr/local/pscal
EOF
chmod 644 "$RFS/etc/profile"

cat > "$RFS/etc/rc" <<'EOF'
#!/usr/bin/sh
export PATH=/usr/bin
export PSCAL_INSTALL_ROOT=/usr/local/pscal
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devpts devpts /dev/pts 2>/dev/null
exec /usr/bin/exsh
EOF
chmod +x "$RFS/etc/rc"

echo "== Step 3/3: package =="
( cd "$RFS" && tar -czf "$OUT_DIR/pscal-pure-aarch64-rootfs.tar.gz" . )
echo "Wrote $OUT_DIR/pscal-pure-aarch64-rootfs.tar.gz"
du -sh "$OUT_DIR/pscal-pure-aarch64-rootfs.tar.gz"
