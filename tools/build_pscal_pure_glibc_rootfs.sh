#!/usr/bin/env bash
# Pure PSCAL/SmallCLUE aarch64 rootfs for iSH-AOK -- glibc, native (no cross-compile,
# no musl). Runs natively inside a Debian aarch64 container on Apple Silicon, using
# apt-supplied -dev packages and each vendored dependency's OWN build system (real
# ./configure for OpenSSH, real CMake for libgit2) -- the same pattern the iOS build
# and the earlier i386 iSH port both use, just with a target glibc actually knows.
set -euo pipefail
OUT_DIR="${1:-/out}"
# CRITICAL: all build/assembly work happens under a CONTAINER-LOCAL path,
# never under $OUT_DIR directly. $OUT_DIR is a Docker bind mount back to
# the macOS host, and Docker Desktop's bind-mount layer does NOT preserve
# per-container UID/GID -- every file written there, no matter which uid
# inside the container wrote it, shows up host-side (and to any process
# that later reads it back through the same mount, including our own
# `tar` step if we're not careful) owned by the HOST's logged-in user, not
# root. That silently broke every rootfs built by an earlier version of
# this script: `/var/empty` (and everything else) shipped owned by uid
# 501/gid 20 (the macOS dev user) instead of uid 0, which is invisible
# until something does a real ownership check (sshd's privsep chroot
# check: "must be owned by root"; getpwuid(stored_uid) in `ls` resolving
# to "?" instead of a name). Fix: assemble AND package the whole rootfs
# under $LOCAL_OUT (pure container-local storage, no bind mount involved,
# so root really is uid 0 all the way through `tar`), and only copy the
# single FINISHED, already-correctly-owned .tar.xz across the bind mount
# at the very end -- a compressed file's own host-side ownership doesn't
# matter, only the per-entry uid/gid bytes already baked into its tar
# headers do.
LOCAL_OUT=/build-out
mkdir -p "$LOCAL_OUT/bin" "$LOCAL_OUT/rootfs" "$OUT_DIR/logs"

echo "== Step 1/3: build 5 PSCAL frontends + SmallCLUE natively (aarch64 glibc, static) =="
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/tmp/apt.log 2>&1
apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config curl wget python3 patch openssh-client xz-utils \
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
  find "/work/$repo/build" -maxdepth 1 -type f -executable -name "$repo" -exec cp {} "$LOCAL_OUT/bin/$repo" \;
  [ -f "$LOCAL_OUT/bin/$repo" ] || { echo "MISSING BINARY $repo"; exit 1; }
done

echo "=== building smallclue (native aarch64 glibc via setup_posix_env.sh) ==="
# Pinned to a known-good commit rather than floating `main` -- main is
# actively developed concurrently (by the user, in parallel sessions) and
# has broken in between builds before (e.g. commit 12a084d "Add chroot
# applet" landed a table entry with no linked implementation). Bump this
# deliberately, not implicitly.
SMALLCLUE_PIN=5fb94fa
git clone https://github.com/emkey1/smallclue.git /work/smallclue \
  >/tmp/clone-smallclue.log 2>&1 || (tail -80 /tmp/clone-smallclue.log; exit 1)
( cd /work/smallclue && git checkout -q "$SMALLCLUE_PIN" ) \
  || { echo "FATAL: could not check out smallclue pin $SMALLCLUE_PIN"; exit 1; }
( cd /work/smallclue && AUTO_INSTALL_DEPS=1 bash setup_posix_env.sh >/tmp/setup-posix.log 2>&1 \
    || (echo "SETUP_POSIX_ENV FAIL"; tail -200 /tmp/setup-posix.log; exit 1) )
cp /work/smallclue/smallclue "$LOCAL_OUT/bin/smallclue"

# setup_posix_env.sh's OpenSSH build step also runs `make sshd` (real, full
# server-side OpenSSH, not a stub) but never installs or applet-wires the
# result -- it's just left sitting in the OpenSSH build tree. Grab it.
if [ ! -f /work/smallclue/third-party/openssh/sshd ]; then
  echo "FATAL: sshd was not built by setup_posix_env.sh"; exit 1
fi
cp /work/smallclue/third-party/openssh/sshd "$LOCAL_OUT/bin/sshd"

for f in $FRONTENDS smallclue sshd; do
  file "$LOCAL_OUT/bin/$f" | grep -q "statically linked" || { echo "FATAL: $f not statically linked"; file "$LOCAL_OUT/bin/$f"; exit 1; }
  file "$LOCAL_OUT/bin/$f" | grep -qi "aarch64" || { echo "FATAL: $f not aarch64"; file "$LOCAL_OUT/bin/$f"; exit 1; }
done
echo "All 7 binaries built, statically linked, aarch64 glibc."

echo "== Step 2/3: assemble pure PSCAL/SmallCLUE rootfs (merged-usr, no Alpine, no login wrapper) =="
RFS="$LOCAL_OUT/rootfs"
rm -rf "$RFS"
mkdir -p "$RFS/usr/bin" "$RFS/usr/local/pscal/bin" "$RFS/usr/local/pscal/pascal/lib" \
         "$RFS/usr/local/pscal/clike/lib" "$RFS/usr/local/lib/rea" "$RFS/etc/ssh" \
         "$RFS/etc/service/sshd" \
         "$RFS/tmp" "$RFS/var/empty" "$RFS/run" "$RFS/home/username" "$RFS/dev/shm" "$RFS/dev/pts" \
         "$RFS/proc" "$RFS/sys" "$RFS/root/.ssh"
chmod 1777 "$RFS/tmp"
chmod 700 "$RFS/root/.ssh"
# sshd's privsep chroot target: must be root-owned, not group/world-writable
# (sshd refuses to start otherwise -- "must be owned by root and not group
# or world-writable"). Root-owned is automatic since this build runs as
# root inside the container; chmod defensively regardless of base-image
# umask defaults.
chmod 0711 "$RFS/var/empty"
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

cp "$LOCAL_OUT/bin/smallclue" "$RFS/usr/bin/smallclue"
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
  cp "$LOCAL_OUT/bin/$f" "$RFS/usr/local/pscal/bin/$f"
  chmod +x "$RFS/usr/local/pscal/bin/$f"
  ln -sf /usr/local/pscal/bin/$f "$RFS/usr/bin/$f"
done

# sshd: real binary, not a smallclue applet symlink (it's built by OpenSSH's
# own Makefile, not smallclue's multicall dispatch).
cp "$LOCAL_OUT/bin/sshd" "$RFS/usr/bin/sshd"
chmod +x "$RFS/usr/bin/sshd"

cp /pbuild-lib/pascal/*.pl "$RFS/usr/local/pscal/pascal/lib/" 2>/dev/null || true
cp /pbuild-lib/clike/*.cl "$RFS/usr/local/pscal/clike/lib/" 2>/dev/null || true
find /pbuild-lib/rea -maxdepth 1 -type f ! -iname "README.md" -exec cp {} "$RFS/usr/local/lib/rea/" \; 2>/dev/null || true

cat > "$RFS/etc/passwd" <<'EOF'
root:x:0:0:root:/root:/usr/bin/exsh
username:x:1000:1000:User Name,,,:/home/username:/usr/bin/exsh
sshd:x:100:100:sshd privilege separation:/var/empty:/usr/bin/false
nobody:x:65534:65534:nobody:/nonexistent:/usr/bin/false
EOF
cat > "$RFS/etc/group" <<'EOF'
root:x:0:
username:x:1000:
sshd:x:100:
nogroup:x:65534:
EOF
# /etc/shadow: POSIX only specifies the FILE FORMAT, not which accounts must
# exist -- that's a distro convention (LSB/Debian's base-passwd). A full
# Debian-style base account list (daemon/bin/sys/mail/news/uucp/...) would
# just be clutter here (no init system, no mail, no printing); nobody/
# nogroup (65534) is the one genuinely universal least-privilege account
# worth adding regardless. Every account is LOCKED (`*`) by default --
# matches sshd_config's PasswordAuthentication no below; smallclue's own
# `passwd` applet (getspnam/crypt against this same file) is how the user
# sets a real password locally if they ever want one. No password-aging
# fields set (blank = disabled), matching common minimal-image convention.
cat > "$RFS/etc/shadow" <<'EOF'
root:*:::::::
username:*:::::::
sshd:*:::::::
nobody:*:::::::
EOF
chmod 600 "$RFS/etc/shadow"
cat > "$RFS/etc/hosts" <<'EOF'
127.0.0.1   localhost
::1         localhost ip6-localhost ip6-loopback
EOF
cat > "$RFS/etc/hostname" <<'EOF'
pscal-ish
EOF

cat > "$RFS/etc/profile" <<'EOF'
export PATH=/usr/bin
export PSCAL_INSTALL_ROOT=/usr/local/pscal
export PS1='\u@\h:\w\$ '
EOF
chmod 644 "$RFS/etc/profile"

# sshd_config: key-only auth by default. There is no /etc/shadow in this
# rootfs at all, so password auth has nothing real to check against anyway
# -- PermitRootLogin prohibit-password + PasswordAuthentication no means
# sshd starts on boot but nobody gets in until the user drops a public key
# into /root/.ssh/authorized_keys themselves (never an accidentally-open
# passwordless root login). No UsePAM directive at all -- this static
# OpenSSH build has no PAM support compiled in, so the option is flatly
# UNRECOGNIZED (not just a no-op) and sshd refuses to start with it present.
cat > "$RFS/etc/ssh/sshd_config" <<'EOF'
Port 22
PermitRootLogin prohibit-password
PasswordAuthentication no
PermitEmptyPasswords no
HostKey /etc/ssh/ssh_host_rsa_key
HostKey /etc/ssh/ssh_host_ecdsa_key
HostKey /etc/ssh/ssh_host_ed25519_key
AuthorizedKeysFile /root/.ssh/authorized_keys
Subsystem sftp internal-sftp
EOF

# /etc/service/sshd/run: the runit convention smallclue's own `runit` applet
# expects (NOT SysV's /etc/init.d -- this rootfs has no runlevels, no
# update-rc.d, just a single /etc/rc). Host-key generation and the
# sshd.disable opt-out both live HERE, not in /etc/rc, so a future second
# service just drops its own /etc/service/<name>/run in -- no /etc/rc edits.
# sshd runs with -D (stay in foreground) because runit's own reap loop
# expects to own the child directly, matching daemontools/runit convention
# (services are supervised in the foreground, not self-daemonizing) even
# though our minimal runit doesn't actually restart a crashed service yet.
cat > "$RFS/etc/service/sshd/run" <<'EOF'
#!/usr/bin/sh
if [ -f /etc/ssh/sshd.disable ]; then
    exit 0
fi
if [ ! -f /etc/ssh/ssh_host_rsa_key ]; then
    ssh-keygen -t rsa -f /etc/ssh/ssh_host_rsa_key -N "" -q
    ssh-keygen -t ecdsa -f /etc/ssh/ssh_host_ecdsa_key -N "" -q
    ssh-keygen -t ed25519 -f /etc/ssh/ssh_host_ed25519_key -N "" -q
fi
exec /usr/bin/sshd -D
EOF
chmod +x "$RFS/etc/service/sshd/run"

cat > "$RFS/etc/rc" <<'EOF'
#!/usr/bin/sh
export PATH=/usr/bin
export PSCAL_INSTALL_ROOT=/usr/local/pscal
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devpts devpts /dev/pts 2>/dev/null

hostname pscal-ish 2>/dev/null

# runit's own reap loop blocks forever (by design, matching daemontools),
# so it must be backgrounded here. It's forked as its own process before
# rc execs into exsh below, so when that exec happens runit is simply
# reparented to init -- init's own waitpid(-1) reap loop already handles
# orphans like this correctly.
runit &

# NOT `exec /usr/bin/exsh` -- on real device hardware (not reproduced via
# ish-cli on macOS, which has none of the same memory pressure) the VERY
# FIRST exsh reached via an in-place exec (replacing this "sh" process
# image directly, same pid, no new fork) has its interactive prompt
# formatting fail wholesale (shellFormatPrompt falls back to the raw,
# unexpanded PS1 string) -- 100% reproducible, every cold boot, but ONLY
# for this exec-in-place path. Manually re-running `exsh` as an ordinary
# command from inside that broken shell (a real fork+exec, not an
# in-place exec) always works correctly. Plain (non-exec) invocation here
# reproduces the working fork+exec path from the very first launch,
# avoiding whatever's specific to cold-boot in-place exec (memory
# pressure from init/mounts/runit/sshd/JIT-warmup all happening
# concurrently is the leading theory, unconfirmed).
export PS1='\u@\h:\w\$ '
/usr/bin/exsh
exit $?
EOF
chmod +x "$RFS/etc/rc"

echo "== Step 3/3: package as .tar.xz, container-local, then copy the finished archive out =="
# .tar.xz (not .tar.gz + a separate host-side xz repack) specifically so
# there is NO intermediate extract/recompress step on the macOS host --
# that would silently reintroduce the exact same host-user-ownership bug
# this whole restructure exists to avoid (non-root extraction on macOS
# can't preserve root ownership either, same underlying limitation).
( cd "$RFS" && XZ_OPT=-9 tar -cJf "$LOCAL_OUT/pscal-pure-aarch64-rootfs.tar.xz" . )
cp "$LOCAL_OUT/pscal-pure-aarch64-rootfs.tar.xz" "$OUT_DIR/pscal-pure-aarch64-rootfs.tar.xz"
echo "Wrote $OUT_DIR/pscal-pure-aarch64-rootfs.tar.xz"
du -sh "$OUT_DIR/pscal-pure-aarch64-rootfs.tar.xz"
echo "--- ownership sanity check (must show uid=0 gid=0, not the host user) ---"
tar -tvf --numeric-owner "$LOCAL_OUT/pscal-pure-aarch64-rootfs.tar.xz" 2>/dev/null | grep "var/empty" || true
