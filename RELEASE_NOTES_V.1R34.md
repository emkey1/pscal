# PSCAL V.1R34 Release Notes

Base: `V.1R33`

## New in the iOS app
- `ip`, iproute2's command for the device's network, in Linux's layout and words: `ip a` (`ip address show [dev NAME]`), `ip link`, `ip route` and `ip route get ADDRESS`, with `-4`, `-6`, `-0`, `-br`, `-o`, `-j` and `-p`. The loopback interface is `lo`, as on Linux; the others keep iOS's names (`en0`, `pdp_ip0`, `utun0`).

## Fixed in the iOS app
- `ping` gets replies. Every request went out with its checksum's two bytes swapped, so every host, a LAN host, the router and the app's own loopback alike, dropped it as corrupt. Each packet then ended in `Request timeout for icmp_seq N (Resource temporarily unavailable)`, with 100% loss.
- `df` shows the app's own filesystems: the root as `rootfs` on `/`, with the size and free space of the app's storage, and anything mounted in the app. It read the device's mount table, so it printed `No such file or directory` for each of the device's own volumes and listed the rest on `/` under Apple's volume names. `df`, `df -h`, `df /` and `df .` agree.
- `/proc/mounts`, `/proc/mountinfo`, `mount` and `stat -f` read the same table: the root is an `ext4` filesystem called `rootfs`, `/proc` is a `proc` filesystem with no blocks, a folder mounted from inside the app's storage is on `rootfs`, and one from outside it is `external` (`fuse`). No host path or volume name shows. `stat -f /` said `UNKNOWN (0x1a)`, `stat -f /proc` said `No such file or directory`, and a bare `mount` printed `/proc/mounts` lines; it prints util-linux's `SRC on DIR type TYPE (OPTS)` as on Linux.
- `mount -p` writes `/etc/fstab` in the app's own terms: a folder in the app's storage by its path there, and one from outside it, such as a folder from the picker, as `UUID=...`, a label the app keeps for that folder. It wrote the device's path for both. A line with a device path from an earlier build still mounts at launch.
- `fstatfs` and `fstatvfs` on a descriptor name its filesystem from the same mount table as `statfs`; a terminal is on `devpts` and a pipe on `pipefs`. They gave the device's volume and mount point.
- `/proc/net/dev`, `/proc/net/route`, `/proc/net/if_inet6` and `/proc/net/ipv6_route` come from the same reading as `ip`, and agree with it. `route` wrote its addresses byte-swapped, listed loopback and had no default route; `if_inet6` gave `::1` the wrong scope and kept iOS's scope bits inside link-local addresses; `ipv6_route` was made up; and `dev` counted no drops or multicast.
- An ended ssh session is gone from `ps`, `top`, `lps` and `/proc`. Each ssh tab or window is a child of pid 1, and pid 1 never reaped one, so every session that had ended, a failed connection included, stayed as a zombie of pid 1 until the app restarted, and pid 1 showed `sigchld` for good. Pid 1 now reaps its own children as they end, as init does on Linux.
- No process is left running after a tab or window closes. The `exsh` with no terminal and its own session that `ps` and `top` showed was no shell: it was the app's own process id, given a row when the first tab's shell started. Its "CPU time" was how long the app had been open, a tenth of it shown as system time, which is also where `top`'s 25% usr with nothing running came from. Opening a second tab also renamed the first tab's shell from `shell` to `exsh`. A task whose CPU time can't be measured now shows 0, not its time alive.
- `ipaddr` reads the same data as `ip`, so its loopback is `lo`, as in `ip`, where it said `lo0`, and a link-local address has no `%en0` after it.

## Differs from Linux on iOS
- `ip` changes nothing: `ip link set`, `ip address add` and `ip route add` say `RTNETLINK answers: Operation not permitted` with status 2, as for a normal user on Linux, and root gets the same answer. The app can't configure the device's network.
- `ip` shows every address as `valid_lft forever preferred_lft forever`, without `dynamic` or `temporary`, and prints no `qdisc` or `qlen`: iOS doesn't report them. iOS hides hardware addresses, so `link/ether` shows `02:00:00:00:00:00`.
- `ip neigh`, `ip rule` and the other objects past address, link and route aren't there, nor are `-s`, `-d` and batch mode.
