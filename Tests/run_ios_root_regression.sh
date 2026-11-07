#!/bin/bash
# Root, su and sudo in the iOS-flavoured shell: who a task is, what a normal user may not touch,
# the optional root password, and the host name. The kernel's calls are vproc's, so this host build
# exercises the app's code. It never makes a real setuid call.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXSH="${1:-${ROOT}/build/ios-host/bin/exsh}"

if [ ! -x "${EXSH}" ]; then
  echo "Missing ${EXSH}. Build with: cmake --build build/ios-host --target exsh" >&2
  exit 1
fi
EXSH="$(cd "$(dirname "${EXSH}")" && pwd -P)/$(basename "${EXSH}")"

TEST_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/ios-root.XXXXXX")"
trap 'rm -rf "${TEST_ROOT}"' EXIT
TEST_ROOT="$(cd "${TEST_ROOT}" && pwd -P)"
mkdir -p "${TEST_ROOT}/home" "${TEST_ROOT}/etc" "${TEST_ROOT}/var/root" "${TEST_ROOT}/var/run" "${TEST_ROOT}/tmp"
UID_NOW="$(id -u)"
# The app's accounts, with the login account at this host's uid, as the app has it.
sed "s/^mobile:x:501:501:/mobile:x:${UID_NOW}:501:/" "${ROOT}/etc/passwd" > "${TEST_ROOT}/etc/passwd"
cp "${ROOT}/etc/group" "${ROOT}/etc/shadow" "${ROOT}/etc/sudoers" "${TEST_ROOT}/etc/"
printf 'a\n' > "${TEST_ROOT}/home/f"

export PATH_TRUNCATE="${TEST_ROOT}" PSCALI_ETC_ROOT="${TEST_ROOT}/etc" HOME="${TEST_ROOT}/home"

fails=0
check() {
  local name=$1 cmd=$2 want=$3
  local got
  got="$(cd "${TEST_ROOT}/home" && /usr/bin/perl -e 'alarm 60; exec @ARGV' "${EXSH}" -c "${cmd}" < /dev/null 2>&1)"
  if [ "${got}" = "${want}" ]; then
    echo "[PASS] ${name}"
  else
    echo "[FAIL] ${name}"
    echo "  command: ${cmd}"
    echo "  want: $(printf '%s' "${want}" | od -c | head -5)"
    echo "  got:  $(printf '%s' "${got}" | od -c | head -5)"
    echo "  got text: ${got}"
    fails=$((fails + 1))
  fi
}

check id_login 'id -un; [ "$(id -u)" = "'"${UID_NOW}"'" ] && echo uid-ok; id -g' $'mobile\nuid-ok\n501'
check id_groups 'id -G' $'501 250 286 299'
check sudo_id 'sudo id -u; sudo id -un; sudo id -g; id -un' $'0\nroot\n0\nmobile'
check sudo_vars 'sudo sh -c '"'"'echo $UID $EUID'"'"'; echo $EUID' $'0 0\n'"${UID_NOW}"
check sudo_pipeline 'sudo id -u | cat; id -un' $'0\nmobile'
check sudo_subshell '( sudo true; id -un )' $'mobile'
check sudo_job 'sudo id -u > j & wait; cat j; id -un' $'0\nmobile'
check sudo_exec 'sudo sh -c "exec id -un"; echo still' $'root\nstill'
check sudo_unknown_user 'sudo -u nosuch id 2>&1; echo rc=$?' $'sudo: unknown user: nosuch\nrc=1'
check sudo_k_alone 'sudo -k; echo rc=$?' $'rc=0'
check su_c 'su -c "id -un"; id -un' $'root\nmobile'
check su_stdin 'printf "id -un\nexit 3\n" | su; echo rc=$?; id -un' $'root\nrc=3\nmobile'
check su_dash 'su - -c pwd' $'/var/root'
check su_pscal 'su pscal -c "id -un"' $'pscal'
check su_unknown 'su nosuch 2>&1; echo rc=$?' $'su: user nosuch does not exist\nrc=1'
check etc_write_denied '{ echo x > /etc/foo; } 2>/dev/null; echo rc=$?; [ -e /etc/foo ] || echo absent' $'rc=1\nabsent'
check etc_touch_denied 'touch /etc/foo 2>/dev/null || echo denied; mkdir /etc/d 2>/dev/null || echo denied' $'denied\ndenied'
check etc_rm_denied '( cd /etc && rm -f passwd 2>/dev/null; mv group g2 2>/dev/null ); [ -f /etc/passwd ] && [ -f /etc/group ] && echo kept' $'kept'
check etc_dotdot_denied '{ echo x > /home/../etc/foo; } 2>/dev/null; echo rc=$?' $'rc=1'
check etc_sudo_write 'sudo sh -c "echo x > /etc/foo"; cat /etc/foo; sudo rm /etc/foo; [ -e /etc/foo ] || echo gone' $'x\ngone'
check etc_tee 'echo y | sudo tee /etc/bar > /dev/null; cat /etc/bar; sudo rm /etc/bar' $'y'
check etc_cp_into_denied 'cp f /etc/f 2>/dev/null; echo rc=$?' $'rc=1'
check shadow_unreadable 'cat /etc/shadow > /dev/null 2>&1; echo rc=$?; sudo cut -d: -f1 /etc/shadow | head -1' $'rc=1\nroot'
check shadow_symlink 'ln -s ../etc/shadow sl; cat sl > /dev/null 2>&1; echo rc=$?; rm -f sl' $'rc=1'
check shadow_hardlink 'ln /etc/passwd hl 2>/dev/null; echo rc=$?; rm -f hl' $'rc=1'
check etc_owner 'ls -l /etc/passwd | awk "{print \$3}"; ls -l f | awk "{print \$3}"' $'root\nmobile'
check chown_record 'touch co; chown root co 2>/dev/null; echo rc=$?; sudo chown root co; ls -l co | awk "{print \$3}"' $'rc=1\nroot'
check passwd_flow 'printf "pw1\npw1\n" | sudo passwd > /dev/null 2>&1; sudo -k; sudo -n true 2>/dev/null; echo n=$?; echo bad | sudo -S true 2>/dev/null; echo bad=$?; echo pw1 | sudo -S id -u 2>/dev/null; sudo -n id -un; echo pw1 | su -c "id -un" 2>/dev/null; su -c true < /dev/null 2>/dev/null; echo nopw=$?; sudo passwd -d root > /dev/null; sudo -k; sudo -n id -un' $'n=1\nbad=1\n0\nroot\nroot\nnopw=1\nroot'
check passwd_hash 'printf "pw2\npw2\n" | sudo passwd > /dev/null 2>&1; echo pw2 | sudo -S cut -d: -f2 /etc/shadow 2>/dev/null | head -1 | cut -c1-17; sudo passwd -d root > /dev/null; sudo -k' $'$6$rounds=100000$'
check passwd_self 'printf "me\nme\n" | passwd > /dev/null 2>&1; echo rc=$?; sudo grep -c "^mobile:[$]6[$]" /etc/shadow; sudo passwd -d mobile > /dev/null' $'rc=0\n1'
check hostname_default 'hostname; hostname -s; echo "$HOSTNAME"; uname -n' $'iDevice\niDevice\niDevice\niDevice'
check hostname_set 'hostname foo 2>/dev/null; echo rc=$?; sudo hostname smoke.host; hostname; hostname -s; uname -n' $'rc=1\nsmoke.host\nsmoke\nsmoke.host'

echo "ios root regression: ${fails} failed"
[ "${fails}" -eq 0 ]
