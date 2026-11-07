# PSCAL V.1R34 Release Notes

Base: `V.1R33`

## Fixed in the iOS app
- `ping` gets replies. Every request went out with its checksum's two bytes swapped, so every host, a LAN host, the router and the app's own loopback alike, dropped it as corrupt. Each packet then ended in `Request timeout for icmp_seq N (Resource temporarily unavailable)`, with 100% loss.
- `df` shows the app's own filesystems: the root as `rootfs` on `/`, with the size and free space of the app's storage, and anything mounted in the app. It read the device's mount table, so it printed `No such file or directory` for each of the device's own volumes and listed the rest on `/` under Apple's volume names. `df`, `df -h`, `df /` and `df .` agree.
- `/proc/mounts`, `/proc/mountinfo`, `mount` and `stat -f` read the same table: the root is an `ext4` filesystem called `rootfs`, `/proc` is a `proc` filesystem with no blocks, a folder mounted from inside the app's storage is on `rootfs`, and one from outside it is `external` (`fuse`). No host path or volume name shows. `stat -f /` said `UNKNOWN (0x1a)`, `stat -f /proc` said `No such file or directory`, and a bare `mount` printed `/proc/mounts` lines; it prints util-linux's `SRC on DIR type TYPE (OPTS)` as on Linux.
