#!/usr/bin/env bash
# Pure PSCAL/SmallCLUE aarch64 rootfs for iSH-AOK -- glibc, native (no cross-compile,
# no musl). Runs natively inside a Debian aarch64 container on Apple Silicon, using
# apt-supplied -dev packages and each vendored dependency's OWN build system (real
# ./configure for OpenSSH, real CMake for libgit2) -- the same pattern the iOS build
# and the earlier i386 iSH port both use, just with a target glibc actually knows.
set -euo pipefail
OUT_DIR="${1:-/out}"

# Every published image carries a version, because the iSH-AOK root picker
# shows the two most recent PSCAL builds side by side (see that repo's
# manifest.json "series"/"version" keys) and a user needs to be able to tell
# which one they are looking at, both in the picker and from inside the guest
# (/etc/pscal-release). Dotted date, not a counter: the interesting question
# about a rootfs build is always "how old is it".
PSCAL_ROOTFS_VERSION="${PSCAL_ROOTFS_VERSION:-$(date -u +%Y.%m.%d)}"
ARCHIVE_NAME="pscal-rootfs-${PSCAL_ROOTFS_VERSION}-aarch64"
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
    zlib1g-dev libssl-dev libncurses-dev libc-ares-dev autoconf automake libtool file \
    netbase \
    >>/tmp/apt.log 2>&1 || (tail -100 /tmp/apt.log; exit 1)
echo APT_OK

echo "=== building static libcurl (HTTP only, OpenSSL, c-ares) ==="
# aether forces PSCAL_CURL=ON, so pscal-core's find_package(CURL REQUIRED) is
# now a hard dependency of one of the five frontends. Debian's own libcurl.a
# is not usable here: `pkg-config --static --libs libcurl` drags in nghttp2,
# idn2, rtmp, ssh2, psl, ldap/lber, krb5, zstd and brotli, and several of
# those have no static archive in the archive at all. Build the libcurl this
# image actually needs instead -- HTTP/HTTPS, OpenSSL, zlib, nothing else --
# from the curl release PSCAL already vendors, so the guest's HTTP stack is
# the same version PSCAL ships on iOS.
#
# --enable-ares is the load-bearing flag. Everything here is statically
# linked against glibc, and static glibc's getaddrinfo() resolves DNS names by
# dlopen()ing libnss_dns.so.2 at runtime -- a shared object this rootfs does
# not contain and never will. c-ares speaks DNS itself, reading the
# /etc/resolv.conf that iSH-AOK writes into the guest on every network change,
# so hostnames resolve instead of failing the moment anything is fetched.
mkdir -p /work/curl-build
( cd /work/curl-build && /pbuild-curl/configure \
    --prefix=/usr/local --disable-shared --enable-static \
    --with-openssl --with-zlib --enable-ares \
    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt \
    --without-libpsl --without-libidn2 --without-nghttp2 --without-brotli \
    --without-zstd --without-librtmp --without-libssh2 --without-libssh \
    --disable-ldap --disable-ldaps --disable-docs \
    --disable-ftp --disable-file --disable-dict --disable-telnet --disable-tftp \
    --disable-pop3 --disable-imap --disable-smb --disable-smtp --disable-gopher \
    --disable-mqtt --disable-rtsp \
    >/tmp/curl-configure.log 2>&1 \
    || (echo "CONFIGURE FAIL curl"; tail -60 /tmp/curl-configure.log; exit 1) )
( cd /work/curl-build && make -j"$(nproc)" >/tmp/curl-build.log 2>&1 \
    && make install >>/tmp/curl-build.log 2>&1 \
    || (echo "BUILD FAIL curl"; tail -80 /tmp/curl-build.log; exit 1) )
[ -f /usr/local/lib/libcurl.a ] || { echo "FATAL: libcurl.a not installed"; exit 1; }
# FindCURL hands the consumer libcurl.a and nothing else, so the transitive
# static dependencies have to be named somewhere the linker sees them AFTER
# it. CMAKE_C_STANDARD_LIBRARIES is the end of the link line; linker flags
# would land before the objects and be dropped.
CURL_STATIC_DEPS="-lssl -lcrypto -lcares -lz"

FRONTENDS="pascal aether rea clike exsh"
# Provenance: which commit of each component this image was actually built
# from. Written into the rootfs as /etc/pscal-release in step 2 -- a bug
# report from a device is useless without it, and "main at some point" is not
# an answer anyone can act on.
PROVENANCE=""
for repo in $FRONTENDS; do
  echo "=== building $repo ==="
  if [ "$repo" = "aether" ]; then
    git clone --depth 1 --recurse-submodules "https://github.com/emkey1/$repo.git" "/work/$repo" \
      >/tmp/clone-$repo.log 2>&1 || (tail -80 /tmp/clone-$repo.log; exit 1)
  else
    git clone --depth 1 "https://github.com/emkey1/$repo.git" "/work/$repo" \
      >/tmp/clone-$repo.log 2>&1 || (tail -50 /tmp/clone-$repo.log; exit 1)
  fi
  # All five get libcurl, not just aether. pscal-core's PSCAL_CURL defaults to
  # OFF and only aether forced it ON, which left the image shipping
  # lib/rea/http and lib/clike/http.cl as dead weight -- a rea program calling
  # Http.get died with "vmBuiltinHttpSession is unavailable: this build omits
  # libcurl-based networking".
  # An array, because CMAKE_C_STANDARD_LIBRARIES is one argument containing
  # spaces -- an unquoted string here splits at them and cmake reports
  # "Unknown argument -lz".
  EXTRA_CMAKE_ARGS=(
    "-DPSCAL_CURL=ON"
    "-DCMAKE_C_STANDARD_LIBRARIES=$CURL_STATIC_DEPS"
    # Stamps `<frontend> -v` with the image it belongs to. The frontends take
    # this rather than running `git describe` because the clones above are
    # --depth 1 and a shallow clone has no tags; the matching git tag is pushed
    # to each repo when the image is published.
    "-DPSCAL_ROOTFS_TAG=pscal-rootfs-$PSCAL_ROOTFS_VERSION"
  )
  ( cd "/work/$repo" && mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-static" -DCMAKE_EXE_LINKER_FLAGS="-static" \
      "${EXTRA_CMAKE_ARGS[@]}" \
      >/tmp/cmake-$repo.log 2>&1 || (echo "CMAKE FAIL $repo"; tail -100 /tmp/cmake-$repo.log; exit 1) )
  # -DPSCAL_CURL=ON wins only because nothing in these trees set()s it FORCEd
  # first, and a frontend that started doing so would go quietly back to the
  # stub: a binary that builds, links and runs, and fails only once someone
  # asks it to fetch something. pscal-core announces the real backend itself;
  # insist on hearing it rather than trusting the flag.
  grep -q "pscal-core: libcurl networking ENABLED" /tmp/cmake-$repo.log \
    || { echo "FATAL: $repo configured WITHOUT libcurl networking despite -DPSCAL_CURL=ON"; exit 1; }
  ( cd "/work/$repo/build" && cmake --build . -j"$(nproc)" >/tmp/build-$repo.log 2>&1 \
      || (echo "BUILD FAIL $repo"; tail -150 /tmp/build-$repo.log; exit 1) )
  find "/work/$repo/build" -maxdepth 1 -type f -executable -name "$repo" -exec cp {} "$LOCAL_OUT/bin/$repo" \;
  [ -f "$LOCAL_OUT/bin/$repo" ] || { echo "MISSING BINARY $repo"; exit 1; }
  PROVENANCE="${PROVENANCE}${repo} $(git -C "/work/$repo" rev-parse --short=12 HEAD)
"
done

echo "=== building smallclue (native aarch64 glibc via setup_posix_env.sh) ==="
# Pinned to a known-good commit rather than floating `main` -- main is
# actively developed concurrently (by the user, in parallel sessions) and
# has broken in between builds before (e.g. commit 12a084d "Add chroot
# applet" landed a table entry with no linked implementation). Bump this
# deliberately, not implicitly.
SMALLCLUE_PIN="${SMALLCLUE_PIN:-97be946}"
# --recurse-submodules, not a plain clone: smallclue's third-party deps
# (openssh, libgit2, dvtm, nextvi, openrsync) are submodules now, and
# fetch_dependencies.sh only knows how to re-download the ones that still have
# a tarball fallback. A plain clone leaves the rest as empty directories and
# the build fails several minutes later complaining about a missing ssh.c.
git clone --recurse-submodules https://github.com/emkey1/smallclue.git /work/smallclue \
  >/tmp/clone-smallclue.log 2>&1 || (tail -80 /tmp/clone-smallclue.log; exit 1)
( cd /work/smallclue && git checkout -q "$SMALLCLUE_PIN" \
    && git submodule update --init --recursive ) \
  >>/tmp/clone-smallclue.log 2>&1 \
  || { echo "FATAL: could not check out smallclue pin $SMALLCLUE_PIN"; tail -40 /tmp/clone-smallclue.log; exit 1; }
PROVENANCE="${PROVENANCE}smallclue $(git -C /work/smallclue rev-parse --short=12 HEAD)
"

# git gives every file it writes the same checkout timestamp, in no particular
# order, and OpenSSH's configure refuses to run at all if configure.ac or any
# m4/*.m4 comes out newer than the generated configure ("newer than configure,
# run autoreconf"). Whether a fresh clone builds is therefore a coin toss.
# Restamp the generated files in dependency order -- one touch per line, since
# `touch a b` gives both the SAME time and the comparison is strictly-newer.
# Restamping beats running autoreconf: this tree is patched (ssh.c's main is
# renamed for embedding) and regenerating it with whatever autoconf the base
# image happens to ship invites a different failure.
OPENSSH_DIR=/work/smallclue/third-party/openssh

# Put upstream OpenSSH's plain globals back for this build. The fork spells
# ~240 of them `__thread` so that AOK can run ssh as a NATIVE PROGRAM inside
# the app, where the process is shared and every guest task needs its own
# copy of the globals. Nothing in this image is embedded that way: smallclue
# and sshd are ordinary executables the guest forks and execs, one process per
# invocation, so thread-locality buys nothing here -- and it costs the whole
# build, because the server side does not compile with it. auth2-methods.c
# initialises static structs with &options.password_authentication and
# friends, and the address of a thread-local is not a constant expression:
# `make sshd` dies on six of them, unguarded, taking setup_posix_env.sh and
# therefore the entire rootfs down with it.
#
# Every occurrence in this tree is a declaration specifier -- none in a string
# or a comment -- so deleting the keyword is exactly the upstream spelling.
find "$OPENSSH_DIR" \( -name '*.c' -o -name '*.h' \) \
    -exec sed -i 's/\bextern __thread\b/extern/g; s/\bstatic __thread\b/static/g; s/\b__thread \b//g' {} +
if grep -rq "__thread" "$OPENSSH_DIR" --include='*.c' --include='*.h'; then
  echo "FATAL: __thread survived in the OpenSSH tree"
  grep -rn "__thread" "$OPENSSH_DIR" --include='*.c' --include='*.h' | head -5
  exit 1
fi

touch "$OPENSSH_DIR/configure.ac"
touch "$OPENSSH_DIR"/m4/*.m4
touch "$OPENSSH_DIR/aclocal.m4"
touch "$OPENSSH_DIR/configure"
touch "$OPENSSH_DIR/config.h.in"
touch "$OPENSSH_DIR/Makefile.in"
# smallclue main has not linked on Linux since df5c449: that commit added
# src/spawn.c and moved core.c's fork+exec sites onto smallclueSpawn/
# smallclueSpawnSimple, but build_smallclue.sh's source list is explicit, not a
# glob, and the new file was never added to it -- so the link ends in six
# undefined references after two hours of compiling. One line, in the same
# alphabetical place the rest of the list uses. Drop this once the fix lands
# upstream; the guard makes it a no-op the moment it does.
if ! grep -q 'src/spawn\.c' /work/smallclue/build_smallclue.sh; then
  sed -i 's|^\([[:space:]]*\)src/split_app\.c \\|\1src/spawn.c \\\n\1src/split_app.c \\|' \
      /work/smallclue/build_smallclue.sh
  grep -q 'src/spawn\.c' /work/smallclue/build_smallclue.sh \
      || { echo "FATAL: could not add src/spawn.c to build_smallclue.sh's source list"; exit 1; }
fi

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

# Modern OpenSSH (post-9.8ish) splits the per-connection handler out of
# sshd into a separate re-exec helper binary, sshd-session, at a path
# hardcoded into sshd itself at compile time via _PATH_SSHD_SESSION
# (-D_PATH_SSHD_SESSION=\"/usr/local/libexec/sshd-session\" in
# setup_posix_env.sh's own OPENSSH_CPPFLAGS -- not something sshd_config
# can override). Without it, sshd's listener starts fine (matching what
# was tested so far) but every real connection fails immediately with
# "/usr/local/libexec/sshd-session does not exist or is not executable".
# ...and 10.0 split the AUTHENTICATION phase out too, into sshd-auth. Both
# published PSCAL images shipped sshd-session alone, so sshd started, listened,
# accepted a TCP connection and then died with "/usr/local/libexec/sshd-auth
# does not exist or is not executable" -- and `sshd -t` passes throughout,
# because validating the config never execs a helper. ssh-sk-helper (FIDO) and
# ssh-pkcs11-helper (smartcards) are the same shape of dependency on the client
# side, so build the lot.
SSHD_HELPERS="sshd-session sshd-auth ssh-sk-helper ssh-pkcs11-helper ssh-keysign"
( cd /work/smallclue/third-party/openssh && make -j4 $SSHD_HELPERS \
    >/tmp/build-sshd-helpers.log 2>&1 \
    || (echo "BUILD FAIL sshd helpers"; tail -100 /tmp/build-sshd-helpers.log; exit 1) )
for h in $SSHD_HELPERS; do
  if [ ! -f "/work/smallclue/third-party/openssh/$h" ]; then
    echo "FATAL: $h was not built"; exit 1
  fi
  cp "/work/smallclue/third-party/openssh/$h" "$LOCAL_OUT/bin/$h"
done

for f in $FRONTENDS smallclue sshd $SSHD_HELPERS; do
  file "$LOCAL_OUT/bin/$f" | grep -q "statically linked" || { echo "FATAL: $f not statically linked"; file "$LOCAL_OUT/bin/$f"; exit 1; }
  file "$LOCAL_OUT/bin/$f" | grep -qi "aarch64" || { echo "FATAL: $f not aarch64"; file "$LOCAL_OUT/bin/$f"; exit 1; }
done
echo "All 7 binaries built, statically linked, aarch64 glibc."

echo "== Step 2/3: assemble pure PSCAL/SmallCLUE rootfs (merged-usr, no Alpine, no login wrapper) =="
RFS="$LOCAL_OUT/rootfs"
rm -rf "$RFS"
mkdir -p "$RFS/usr/bin" "$RFS/usr/local/pscal/bin" "$RFS/usr/local/pscal/pascal/lib" \
         "$RFS/usr/local/pscal/clike/lib" "$RFS/usr/local/lib/rea" "$RFS/etc/ssh" \
         "$RFS/etc/ssl/certs" \
         "$RFS/etc/service/sshd" \
         "$RFS/tmp" "$RFS/var/empty" "$RFS/var/log" "$RFS/run" "$RFS/home/username" "$RFS/dev/shm" "$RFS/dev/pts" \
         "$RFS/proc" "$RFS/sys" "$RFS/root/.ssh"
chmod 1777 "$RFS/tmp"

# sshd stats these FILES and will not create them, so an empty /var/log is not
# enough -- it logged "lastlog_openseek: Couldn't stat /var/log/lastlog" on
# every single login until these existed, and logout() failed for want of
# utmp. Distros ship them empty for exactly this reason. Modes follow Debian's,
# minus the utmp group this rootfs has no reason to invent.
: > "$RFS/var/log/lastlog";  chmod 0644 "$RFS/var/log/lastlog"
: > "$RFS/var/log/wtmp";     chmod 0644 "$RFS/var/log/wtmp"
: > "$RFS/var/log/btmp";     chmod 0600 "$RFS/var/log/btmp"
: > "$RFS/run/utmp";         chmod 0644 "$RFS/run/utmp"
chmod 700 "$RFS/root/.ssh"
# sshd's privsep chroot target: must be root-owned, not group/world-writable
# (sshd refuses to start otherwise -- "must be owned by root and not group
# or world-writable"). Root-owned is automatic since this build runs as
# root inside the container; chmod defensively regardless of base-image
# umask defaults.
chmod 0711 "$RFS/var/empty"
# /var/run -> /run, the merged-run convention that matches this image's
# merged-usr layout. Without it sshd cannot write /var/run/sshd.pid, and
# glibc's _PATH_UTMP (/var/run/utmp) does not resolve either, so every logout
# reported "syslogin_perform_logout: logout() returned an error" and `who`
# and `last` had nothing to read. One symlink, both symptoms.
ln -s ../run "$RFS/var/run"
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
# su, sudo and passwd go to the NATIVE programs, not the applet symlinks the
# loop above just made. SmallCLUE is a multicall binary: /usr/bin/smallclue
# cannot be setuid-root, because it picks its applet from argv[0] and a setuid
# copy would run `sh` as root for anyone. iSH-AOK ships these three as separate
# native programs that can be setuid precisely because they can only ever be
# themselves -- so without this, PATH finds the emulated applet, it is not
# setuid, and the only answer a user ever gets is "must be setuid root".
#
# A wrapper rather than a bare symlink so the failure is graceful: on an
# iSH-AOK too old to carry these, /AOK/native/sudo does not exist and a
# dangling symlink would say "not found", where this falls back to the applet
# and its own clearer message. The wrapper is NOT setuid and does not need to
# be -- the privilege comes from exec'ing the native program, which carries
# the bit itself.
for suid_applet in su sudo passwd; do
  rm -f "$RFS/usr/bin/$suid_applet"
  cat > "$RFS/usr/bin/$suid_applet" <<WRAPPER
#!/usr/bin/sh
[ -x /AOK/native/$suid_applet ] && exec /AOK/native/$suid_applet "\$@"
exec /usr/bin/smallclue $suid_applet "\$@"
WRAPPER
  chmod 755 "$RFS/usr/bin/$suid_applet"
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

# sshd-session MUST live at exactly this path -- it's compiled into sshd
# itself as a fixed string (_PATH_SSHD_SESSION), not configurable via
# sshd_config.
mkdir -p "$RFS/usr/local/libexec"
for h in $SSHD_HELPERS; do
  cp "$LOCAL_OUT/bin/$h" "$RFS/usr/local/libexec/$h"
  chmod +x "$RFS/usr/local/libexec/$h"
done

# Examples, at the path the earlier musl spike used. That script staged them;
# this one never did, so no published image has ever carried them -- and
# nothing complained, because no frontend looks an examples path up. They are
# reference material, which is exactly why their absence was invisible.
#
# Taken from the CLONES rather than a bind mount of the umbrella checkout, so
# they match the exact commit whose binaries are in this image instead of
# whatever the build host happened to have on disk.
#
# Everything except sdl/: this rootfs has no SDL, and those are 1.6 MB of
# demos that could not run. Excluded by name rather than by listing what to
# include, so a new category -- aether ships showcase/ -- arrives without
# needing an edit here.
for repo in $FRONTENDS; do
  src="/work/$repo/examples"
  if [ ! -d "$src" ]; then
    echo "note: $repo ships no examples/ directory"
    continue
  fi
  dest="$RFS/usr/local/pscal/examples/$repo"
  mkdir -p "$dest"
  cp -r "$src/." "$dest/"
  find "$dest" -type d -name sdl -prune -exec rm -rf {} +
  printf '  examples/%-7s %5s KB  %3s files\n' "$repo" \
      "$(du -sk "$dest" | cut -f1)" "$(find "$dest" -type f | wc -l | tr -d ' ')"
done
if [ -z "$(find "$RFS/usr/local/pscal/examples" -type f 2>/dev/null | head -1)" ]; then
  echo "FATAL: no examples were staged"; exit 1
fi
if find "$RFS/usr/local/pscal/examples" -type d -name sdl | grep -q .; then
  echo "FATAL: sdl examples survived the prune"; exit 1
fi

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
wheel:x:10:
username:x:1000:
sshd:x:100:
nogroup:x:65534:
EOF

# /etc/sudoers. SmallCLUE's sudo authenticates the INVOKING user and then asks
# this file whether they may run the command -- and with no file at all nobody
# is authorised, which is the right default but a poor thing to ship. So ship
# the conventional policy: root, and anyone in wheel. The provisioner puts the
# login it creates into wheel.
#
# gid 10 for wheel is the BSD/Arch/Fedora number. Debian uses 27 for `sudo`
# and has no wheel; this rootfs is not Debian and has no group to collide with,
# so the more widely recognised name and number win.
mkdir -p "$RFS/etc/sudoers.d"
chmod 750 "$RFS/etc/sudoers.d"
cat > "$RFS/etc/sudoers" <<'EOF'
# Who may run what, as whom. See sudoers(5) -- SmallCLUE's sudo implements a
# subset: user and %group entries, host lists, (runas) specs, NOPASSWD/PASSWD,
# ALL or an explicit list of absolute command paths, #includedir, and
# last-match-wins. Aliases, negation and globs are NOT implemented and a line
# using them is skipped rather than guessed at.
#
# You are asked for YOUR OWN password, not root's.
root   ALL=(ALL:ALL) ALL
%wheel ALL=(ALL:ALL) ALL

#includedir /etc/sudoers.d
EOF
chmod 440 "$RFS/etc/sudoers"
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
# aether's libcurl was configured with --with-ca-bundle pointing here, so a
# rootfs without this file can open a TLS connection and then refuse every
# certificate on it. Debian's bundle, copied as-is.
cp /etc/ssl/certs/ca-certificates.crt "$RFS/etc/ssl/certs/ca-certificates.crt"
chmod 644 "$RFS/etc/ssl/certs/ca-certificates.crt"

# getservbyname/getprotobyname have nothing to read without these, so `ssh
# host ssh` and anything naming a port by name fails in a way that looks like
# a network problem. Plain data files, taken from the build container.
# NOT /etc/nsswitch.conf: these binaries are statically linked, and giving
# glibc an nsswitch.conf invites it to dlopen NSS modules this rootfs does not
# have. Absent, it uses its built-in defaults, which is what we want.
# netbase is what carries them, and the slim base image does not have it --
# which is exactly why this was `|| true` once and shipped an image with
# neither file in it. Copy them for real and say so if they are missing.
for datafile in services protocols; do
  cp "/etc/$datafile" "$RFS/etc/$datafile" \
    || { echo "FATAL: /etc/$datafile missing from the build container (netbase not installed?)"; exit 1; }
  chmod 644 "$RFS/etc/$datafile"
done

# sshd falls back without moduli, but then diffie-hellman-group-exchange is
# simply unavailable; it is a data file the OpenSSH build already produced.
cp /work/smallclue/third-party/openssh/moduli "$RFS/etc/ssh/moduli" 2>/dev/null \
    && chmod 644 "$RFS/etc/ssh/moduli" || echo "note: no moduli in the OpenSSH tree"

cat > "$RFS/etc/hosts" <<'EOF'
127.0.0.1   localhost
::1         localhost ip6-localhost ip6-loopback
EOF
cat > "$RFS/etc/hostname" <<'EOF'
pscal-ish
EOF

# /etc/pscal-release: the image's own identity. The picker in iSH-AOK shows
# the version, but once a root is imported and renamed the only way back to
# "which build is this" is from inside the guest, so the exact commit of
# every component goes here too.
{
  echo "PSCAL_ROOTFS_VERSION=$PSCAL_ROOTFS_VERSION"
  echo "PSCAL_ROOTFS_BUILT=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "PSCAL_ROOTFS_ARCH=aarch64"
  echo "PSCAL_ROOTFS_LIBC=glibc"
  echo
  echo "# component commit"
  printf '%s' "$PROVENANCE"
} > "$RFS/etc/pscal-release"
chmod 644 "$RFS/etc/pscal-release"

# /etc/os-release is what anything generic asks first (including smallclue's
# own `uname`-adjacent reporting and any script a user brings over from a
# Debian root), so answer it rather than letting the lookup fail.
cat > "$RFS/etc/os-release" <<EOF
NAME="PSCAL"
ID=pscal
PRETTY_NAME="PSCAL + SmallCLUE $PSCAL_ROOTFS_VERSION"
VERSION_ID="$PSCAL_ROOTFS_VERSION"
VERSION="$PSCAL_ROOTFS_VERSION"
HOME_URL="https://github.com/emkey1/pscal"
EOF
chmod 644 "$RFS/etc/os-release"

cat > "$RFS/etc/profile" <<'EOF'
export PATH=/usr/bin
export PSCAL_INSTALL_ROOT=/usr/local/pscal
export PS1='\u@\h:\w\$ '
EOF
chmod 644 "$RFS/etc/profile"

# The one-time setup hint, in root's exsh rc.
#
# The provisioner itself lives at /AOK/tools/provision-ultimate-pscal.sh, with
# the Alpine, Devuan and Arch ones -- served by iSH-AOK, never copied into an
# image, so there is one copy and it is the app's. This only points at it, and
# only when it is actually there: an iSH-AOK older than the script has no such
# file, and telling someone to run something that does not exist is worse than
# saying nothing.
#
# NOT /etc/profile: nothing in this image reads it. /etc/rc sets PATH and PS1
# itself and then runs exsh, whose startup file is ~/.exshrc.
cat >> "$RFS/root/.exshrc" <<'EOF'

if [ ! -f /etc/pscal-provisioned ] && [ -f /AOK/tools/provision-ultimate-pscal.sh ]; then
    echo "Set this machine up (login, password, ssh):"
    echo "    sh /AOK/tools/provision-ultimate-pscal.sh"
fi
EOF
chmod 644 "$RFS/root/.exshrc"

# sshd_config: key-only auth by default. There is no /etc/shadow in this
# rootfs at all, so password auth has nothing real to check against anyway
# -- PermitRootLogin prohibit-password + PasswordAuthentication no means
# sshd starts on boot but nobody gets in until the user drops a public key
# into /root/.ssh/authorized_keys themselves (never an accidentally-open
# passwordless root login). No UsePAM directive at all -- this static
# OpenSSH build has no PAM support compiled in, so the option is flatly
# UNRECOGNIZED (not just a no-op) and sshd refuses to start with it present.
#
# AuthorizedKeysFile is RELATIVE, i.e. per-user, and that is the point: it
# used to read /root/.ssh/authorized_keys, an absolute path, so every user's
# keys were looked for in root's file. A key copied to ~/.ssh did nothing,
# and root's file silently authorised logins as anybody.
cat > "$RFS/etc/ssh/sshd_config" <<'EOF'
Port 22
PermitRootLogin prohibit-password
PasswordAuthentication no
PermitEmptyPasswords no
HostKey /etc/ssh/ssh_host_rsa_key
HostKey /etc/ssh/ssh_host_ecdsa_key
HostKey /etc/ssh/ssh_host_ed25519_key
AuthorizedKeysFile .ssh/authorized_keys
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

# /usr/bin/login: the REAL canonical entry point, not /etc/rc. Discovered
# the hard way -- ish-AOK's app has TWO independent shell-launch paths that
# do NOT converge: AppDelegate.m's boot/console path tries
# /usr/bin/login|/bin/login -f root FIRST (before ever falling through to
# the /bin/init candidate our /etc/service+runit design relies on), and
# TerminalViewController.m's separate interactive-session-shell path
# (opened by the user's normal terminal view, NOT the boot/console view)
# ALSO defaults to /bin/login -f root, but execs it with a near-empty
# environment (just TERM) and, if login is missing, falls through its OWN
# candidate list (/bin/sh -l, /usr/bin/sh -l, ...) which lands on
# smallclue's native `sh` applet (NOT exsh) since that's the first
# candidate that exists on this rootfs. smallclue's own `sh` has ZERO
# bash-style PS1 escape expansion anywhere in its line-editing code
# (verified: no \u/\h/\w handling in src/shell/sh_lineedit.c or
# sh_main.c) -- it prints PS1 completely verbatim, which is EXACTLY why
# the interactive terminal (not the console) showed the raw
# `\u@\h:\w\$` text: the user was never running exsh there at all, they
# were running smallclue's own sh with exsh's PS1 syntax it doesn't
# understand. Shipping a real /bin/login makes BOTH app paths converge on
# the exact same environment setup and the exact same exsh session,
# instead of two different shells with two different (and differently
# broken) prompt behaviors. /etc/rc + /bin/init are left in place as a
# secondary path (still fully correct on their own) for any case where
# login is somehow bypassed, but login is now the primary, always-tried-
# first entry point for both boot and interactive sessions.
cat > "$RFS/usr/bin/login" <<'EOF'
#!/usr/bin/sh
# `case`, not `[ "$1" = "-f" ]` -- smallclue's own `test`/`[` misparses a
# leading-dash comparison operand ("-f") as an attempted unary test
# operator, printing "test: syntax error" (verified directly: `[ "$1" =
# "-f" ]` alone fails, `[ -n "$2" ]` alone is fine). `case` sidesteps the
# ambiguity entirely and is the more idiomatic pattern for this anyway.
case "$1" in
    -f) TARGET_USER="${2:-root}" ;;
    *) TARGET_USER="root" ;;
esac
export USER="$TARGET_USER"
export LOGNAME="$TARGET_USER"
export HOME="/root"
export PATH=/usr/bin
export PSCAL_INSTALL_ROOT=/usr/local/pscal
export PS1='\u@\h:\w\$ '
export TERM="${TERM:-xterm-256color}"

# Mounts/hostname/runit only need to happen once per boot, not once per
# interactive session the user opens -- guarded by a flag in /tmp, which
# is fresh on every real boot.
if [ ! -f /tmp/.pscal-boot-ran ]; then
    mount -t proc proc /proc 2>/dev/null
    mount -t sysfs sys /sys 2>/dev/null
    mount -t devpts devpts /dev/pts 2>/dev/null
    hostname pscal-ish 2>/dev/null
    runit &
    touch /tmp/.pscal-boot-ran
fi

cd "$HOME" 2>/dev/null || cd /root
# Plain invocation, not `exec` -- see the matching note in /etc/rc for why
# (100%-reproducible raw-prompt bug on real device hardware for an
# in-place exec into exsh at cold boot, not reproduced via ish-cli/macOS
# testing; unconfirmed root cause, leading theory is iSH-AOK's iOS
# memory-headroom guard refusing an allocation at peak boot pressure).
/usr/bin/exsh
exit $?
EOF
chmod +x "$RFS/usr/bin/login"

# Every helper path compiled into the shipped binaries must exist in the
# rootfs. Derived from the binaries with `strings`, not from a list kept by
# hand: a hand-written list is exactly what was wrong before -- OpenSSH 9.8
# split out sshd-session and the list was updated, 10.0 split out sshd-auth and
# it was not, and nothing noticed for two releases because `sshd -t` validates
# the config without ever exec'ing a helper. When OpenSSH 11 splits something
# else out, this fails the build instead of shipping a server that dies on the
# first connection.
# ssh-askpass is the one referenced path that is legitimately absent: OpenSSH
# has no build target for it (it is an external X11 passphrase prompter), and
# ssh only reaches for it when it has no terminal to ask on. Everything else
# named by a binary is required. Keep this list to things OpenSSH genuinely
# cannot build -- it is the escape hatch that could hide the next sshd-auth.
OPTIONAL_HELPERS="ssh-askpass"
echo "--- checking every compiled-in helper path exists ---"
MISSING_HELPERS=0
for bin in "$RFS/usr/bin/sshd" "$RFS/usr/local/libexec"/* "$RFS/usr/bin/smallclue"; do
  [ -f "$bin" ] || continue
  for want in $(strings "$bin" | grep -oE "/usr/local/libexec/[A-Za-z0-9._-]+" | sort -u); do
    case " $OPTIONAL_HELPERS " in
      *" $(basename "$want") "*) echo "optional, absent by design: $want"; continue ;;
    esac
    if [ ! -x "$RFS$want" ]; then
      echo "MISSING: $want (referenced by ${bin#$RFS})"
      MISSING_HELPERS=1
    fi
  done
done
if [ "$MISSING_HELPERS" -ne 0 ]; then
  echo "FATAL: a binary references a helper this rootfs does not ship"
  exit 1
fi
echo "all referenced helpers present"

echo "== Step 3/3: package as .tar.xz, container-local, then copy the finished archive out =="
# .tar.xz (not .tar.gz + a separate host-side xz repack) specifically so
# there is NO intermediate extract/recompress step on the macOS host --
# that would silently reintroduce the exact same host-user-ownership bug
# this whole restructure exists to avoid (non-root extraction on macOS
# can't preserve root ownership either, same underlying limitation).
( cd "$RFS" && XZ_OPT=-9 tar -cJf "$LOCAL_OUT/${ARCHIVE_NAME}.tar.xz" . )
cp "$LOCAL_OUT/${ARCHIVE_NAME}.tar.xz" "$OUT_DIR/${ARCHIVE_NAME}.tar.xz"
echo "Wrote $OUT_DIR/${ARCHIVE_NAME}.tar.xz"
du -sh "$OUT_DIR/${ARCHIVE_NAME}.tar.xz"
echo "--- ownership sanity check (must show uid=0 gid=0, not the host user) ---"
tar --numeric-owner -tvf "$LOCAL_OUT/${ARCHIVE_NAME}.tar.xz" 2>/dev/null | grep "var/empty" || true
