# PSCAL V.1R34 Release Notes

Base: `V.1R33`

## New in the iOS app
- `ip`, iproute2's command for the device's network, in Linux's layout and words: `ip a` (`ip address show [dev NAME]`), `ip link`, `ip route` and `ip route get ADDRESS`, with `-4`, `-6`, `-0`, `-br`, `-o`, `-j` and `-p`. The loopback interface is `lo`, as on Linux; the others keep iOS's names (`en0`, `pdp_ip0`, `utun0`).

## Fixed in the iOS app
- `ping` gets replies. Every request went out with its checksum's two bytes swapped, so every host, a LAN host, the router and the app's own loopback alike, dropped it as corrupt. Each packet then ended in `Request timeout for icmp_seq N (Resource temporarily unavailable)`, with 100% loss.
- `df` shows the app's own filesystems: the root as `rootfs` on `/`, with the size and free space of the app's storage, and anything mounted in the app. It read the device's mount table, so it printed `No such file or directory` for each of the device's own volumes and listed the rest on `/` under Apple's volume names. `df`, `df -h`, `df /` and `df .` agree.
- `/proc/mounts`, `/proc/mountinfo`, `mount` and `stat -f` read the same table: the root is an `ext4` filesystem called `rootfs`, `/proc` is a `proc` filesystem with no blocks, a folder mounted from inside the app's storage is on `rootfs`, and one from outside it is `external` (`fuse`). No host path or volume name shows. `stat -f /` said `UNKNOWN (0x1a)`, `stat -f /proc` said `No such file or directory`, and a bare `mount` printed `/proc/mounts` lines; it prints util-linux's `SRC on DIR type TYPE (OPTS)` as on Linux.
- `/proc/net/dev`, `/proc/net/route`, `/proc/net/if_inet6` and `/proc/net/ipv6_route` come from the same reading as `ip`, and agree with it. `route` wrote its addresses byte-swapped, listed loopback and had no default route; `if_inet6` gave `::1` the wrong scope and kept iOS's scope bits inside link-local addresses; `ipv6_route` was made up; and `dev` counted no drops or multicast.
- `ipaddr` reads the same data as `ip`, so its loopback is `lo`, as in `ip`, where it said `lo0`, and a link-local address has no `%en0` after it.

## Differs from Linux on iOS
- `ip` changes nothing: `ip link set`, `ip address add` and `ip route add` say `RTNETLINK answers: Operation not permitted` with status 2, as for a normal user on Linux, and root gets the same answer. The app can't configure the device's network.
- `ip` shows every address as `valid_lft forever preferred_lft forever`, without `dynamic` or `temporary`, and prints no `qdisc` or `qlen`: iOS doesn't report them. iOS hides hardware addresses, so `link/ether` shows `02:00:00:00:00:00`.
- `ip neigh`, `ip rule` and the other objects past address, link and route aren't there, nor are `-s`, `-d` and batch mode.
