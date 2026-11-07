/* The app's ip and /proc/net files, from interfaces and a routing table made up
 * here, and once from the live machine. Built by run_ios_vproc_tests.sh. */

#include "common/net_snapshot.h"
#include "ip_app.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/if_var.h>
#include <net/route.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static int failures = 0;

#define CHECK(cond, ...)                                                                                   \
    do {                                                                                                   \
        if (!(cond)) {                                                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                           \
            fprintf(stderr, __VA_ARGS__);                                                                  \
            fputc('\n', stderr);                                                                           \
            failures++;                                                                                    \
        }                                                                                                  \
    } while (0)

/* net_snapshot.c spells out Darwin's rt_msghdr for the iOS SDK, which lacks it. */
_Static_assert(sizeof(struct rt_msghdr) == 92, "rt_msghdr layout");

static struct ifaddrs *nodes[32];
static int node_count = 0;

static struct sockaddr *sin4(const char *text) {
    struct sockaddr_in *sa = calloc(1, sizeof(*sa));
    sa->sin_len = sizeof(*sa);
    sa->sin_family = AF_INET;
    inet_pton(AF_INET, text, &sa->sin_addr);
    return (struct sockaddr *)sa;
}

static struct sockaddr *sin6(const char *text, unsigned embedded_scope) {
    struct sockaddr_in6 *sa = calloc(1, sizeof(*sa));
    sa->sin6_len = sizeof(*sa);
    sa->sin6_family = AF_INET6;
    inet_pton(AF_INET6, text, &sa->sin6_addr);
    if (embedded_scope) {
        sa->sin6_addr.s6_addr[3] = (unsigned char)embedded_scope;
    }
    return (struct sockaddr *)sa;
}

static void add(const char *name, unsigned flags, struct sockaddr *addr, struct sockaddr *mask, struct sockaddr *dst,
                void *data) {
    struct ifaddrs *ifa = calloc(1, sizeof(*ifa));
    ifa->ifa_name = (char *)name;
    ifa->ifa_flags = flags;
    ifa->ifa_addr = addr;
    ifa->ifa_netmask = mask;
    ifa->ifa_dstaddr = dst;
    ifa->ifa_data = data;
    if (node_count > 0) {
        nodes[node_count - 1]->ifa_next = ifa;
    }
    nodes[node_count++] = ifa;
}

static void addLink(const char *name, unsigned flags, unsigned index, unsigned type, const unsigned char *mac,
                    unsigned mtu) {
    struct sockaddr_dl *sdl = calloc(1, sizeof(*sdl) + 32);
    size_t nlen = strlen(name);
    sdl->sdl_len = (unsigned char)(sizeof(*sdl) + 16);
    sdl->sdl_family = AF_LINK;
    sdl->sdl_index = (unsigned short)index;
    sdl->sdl_type = (unsigned char)type;
    sdl->sdl_nlen = (unsigned char)nlen;
    memcpy(sdl->sdl_data, name, nlen);
    if (mac) {
        sdl->sdl_alen = 6;
        memcpy(sdl->sdl_data + nlen, mac, 6);
    }
    struct if_data *d = calloc(1, sizeof(*d));
    d->ifi_type = (unsigned char)type;
    d->ifi_mtu = mtu;
    d->ifi_ibytes = 1000 * index;
    d->ifi_ipackets = 10 * index;
    d->ifi_obytes = 2000 * index;
    d->ifi_opackets = 20 * index;
    add(name, flags, (struct sockaddr *)sdl, NULL, NULL, d);
}

/* One routing message in NET_RT_DUMP's shape: header, then the sockaddrs named
 * in addrs, each rounded up to four bytes. A mask is shortened to its last
 * nonzero byte, with no family, as Darwin sends it. */
static size_t route(unsigned char *buf, int flags, unsigned index, const struct sockaddr *dst,
                    const struct sockaddr *gw, const unsigned char *mask4, int mask_len) {
    struct rt_msghdr *rtm = (struct rt_msghdr *)buf;
    memset(rtm, 0, sizeof(*rtm));
    rtm->rtm_version = RTM_VERSION;
    rtm->rtm_type = RTM_GET;
    rtm->rtm_index = (unsigned short)index;
    rtm->rtm_flags = flags;
    size_t off = sizeof(*rtm);
    const struct sockaddr *list[3] = {dst, gw, NULL};
    for (int i = 0; i < 2; ++i) {
        if (!list[i]) {
            continue;
        }
        rtm->rtm_addrs |= 1 << i;
        memcpy(buf + off, list[i], list[i]->sa_len);
        off += (list[i]->sa_len + 3u) & ~3u;
    }
    if (mask4) {
        rtm->rtm_addrs |= RTA_NETMASK;
        unsigned char *m = buf + off;
        memset(m, 0, 8);
        m[0] = (unsigned char)(4 + mask_len);
        memcpy(m + 4, mask4, (size_t)mask_len);
        off += (m[0] + 3u) & ~3u;
    }
    rtm->rtm_msglen = (unsigned short)off;
    return off;
}

static char *run(const PscalNetSnapshot *snap, const char *args, int *rc, char **err_text) {
    char *argv[32];
    char copy[512];
    int argc = 0;
    snprintf(copy, sizeof(copy), "ip %s", args);
    for (char *save = NULL, *w = strtok_r(copy, " ", &save); w && argc < 31; w = strtok_r(NULL, " ", &save)) {
        argv[argc++] = w;
    }
    argv[argc] = NULL;
    char *out_text = NULL, *err_buf = NULL;
    size_t out_len = 0, err_len = 0;
    FILE *out = open_memstream(&out_text, &out_len);
    FILE *err = open_memstream(&err_buf, &err_len);
    *rc = smallclueIpRun(argc, argv, snap, out, err);
    fclose(out);
    fclose(err);
    if (err_text) {
        *err_text = err_buf;
    } else {
        free(err_buf);
    }
    return out_text;
}

static void expect(const PscalNetSnapshot *snap, const char *args, int want_rc, const char *want_out,
                   const char *want_err) {
    int rc = 0;
    char *err = NULL;
    char *out = run(snap, args, &rc, &err);
    CHECK(rc == want_rc, "ip %s: status %d, wanted %d", args, rc, want_rc);
    CHECK(strcmp(out, want_out) == 0, "ip %s printed\n%s---- wanted\n%s----", args, out, want_out);
    if (want_err) {
        CHECK(strcmp(err, want_err) == 0, "ip %s said\n%s---- wanted\n%s----", args, err, want_err);
    }
    free(out);
    free(err);
}

static int startsWith(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static char *proc(const PscalNetSnapshot *snap, void (*write)(const PscalNetSnapshot *, FILE *)) {
    char *text = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&text, &len);
    write(snap, f);
    fclose(f);
    return text;
}

static void testMadeUp(void) {
    static const unsigned char mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    unsigned up_lo = IFF_UP | IFF_LOOPBACK | IFF_RUNNING | IFF_MULTICAST;
    unsigned up_en = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST | IFF_SIMPLEX;
    unsigned up_tun = IFF_UP | IFF_POINTOPOINT | IFF_RUNNING | IFF_MULTICAST;
    /* getifaddrs's order: a link entry, then its addresses; en0 listed before lo0. */
    addLink("en0", up_en, 4, IFT_ETHER, mac, 1500);
    add("en0", up_en, sin6("fe80::1c:2d3e:4f50:6172", 4), sin6("ffff:ffff:ffff:ffff::", 0), NULL, NULL);
    add("en0", up_en, sin4("192.168.1.23"), sin4("255.255.255.0"), sin4("192.168.1.255"), NULL);
    add("en0", up_en, sin6("2001:db8::23", 0), sin6("ffff:ffff:ffff:ffff::", 0), NULL, NULL);
    addLink("lo0", up_lo, 1, IFT_LOOP, NULL, 16384);
    add("lo0", up_lo, sin4("127.0.0.1"), sin4("255.0.0.0"), NULL, NULL);
    add("lo0", up_lo, sin6("::1", 0), sin6("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", 0), NULL, NULL);
    addLink("utun0", up_tun, 7, IFT_OTHER, NULL, 1380);
    add("utun0", up_tun, sin4("10.8.0.2"), sin4("255.255.255.255"), sin4("10.8.0.1"), NULL);
    addLink("en1", IFF_UP | IFF_BROADCAST | IFF_MULTICAST, 5, IFT_ETHER, mac, 1500);

    unsigned char rt4[2048], rt6[1024];
    size_t len4 = 0, len6 = 0;
    static const unsigned char zero[1] = {0};
    static const unsigned char m24[3] = {0xff, 0xff, 0xff};
    static const unsigned char m16[2] = {0xff, 0xff};
    len4 += route(rt4 + len4, RTF_UP | RTF_GATEWAY | RTF_STATIC, 4, sin4("0.0.0.0"), sin4("192.168.1.1"), zero, 0);
    /* An interface-scoped copy of the default, as Darwin keeps one per interface. */
    len4 += route(rt4 + len4, RTF_UP | RTF_GATEWAY | RTF_IFSCOPE, 4, sin4("0.0.0.0"), sin4("192.168.1.1"), zero, 0);
    /* The interface route for the address's prefix: the kernel route says it. */
    len4 += route(rt4 + len4, RTF_UP | RTF_CLONING, 4, sin4("192.168.1.0"), NULL, m24, 3);
    /* The gateway's cloned entry, and loopback's own route. */
    len4 += route(rt4 + len4, RTF_UP | RTF_HOST | RTF_LLINFO | RTF_WASCLONED, 4, sin4("192.168.1.1"), NULL, NULL, 0);
    len4 += route(rt4 + len4, RTF_UP | RTF_STATIC, 1, sin4("127.0.0.0"), NULL, (const unsigned char[]){0xff}, 1);
    /* A static on-link network of its own, and a route through the tunnel. */
    len4 += route(rt4 + len4, RTF_UP | RTF_STATIC, 4, sin4("169.254.0.0"), NULL, m16, 2);
    len4 += route(rt4 + len4, RTF_UP | RTF_GATEWAY | RTF_STATIC, 7, sin4("10.0.0.0"), sin4("10.8.0.1"),
                  (const unsigned char[]){0xff}, 1);
    /* An IPv6 default through a link-local router, its scope in the address. */
    struct sockaddr *gw6 = sin6("fe80::1", 4);
    struct sockaddr_in6 *any6 = (struct sockaddr_in6 *)sin6("::", 0);
    len6 += route(rt6, RTF_UP | RTF_GATEWAY, 4, (struct sockaddr *)any6, gw6, zero, 0);

    PscalNetSnapshot snap;
    CHECK(pscalNetSnapshotBuild(&snap, nodes[0], rt4, len4, rt6, len6), "build");
    CHECK(snap.link_count == 4, "links %zu", snap.link_count);

    expect(&snap, "a", 0,
           "1: lo: <LOOPBACK,MULTICAST,UP,LOWER_UP> mtu 16384 state UNKNOWN group default\n"
           "    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n"
           "    inet 127.0.0.1/8 scope host lo\n"
           "       valid_lft forever preferred_lft forever\n"
           "    inet6 ::1/128 scope host\n"
           "       valid_lft forever preferred_lft forever\n"
           "4: en0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP group default\n"
           "    link/ether 02:11:22:33:44:55 brd ff:ff:ff:ff:ff:ff\n"
           "    inet 192.168.1.23/24 brd 192.168.1.255 scope global en0\n"
           "       valid_lft forever preferred_lft forever\n"
           "    inet6 2001:db8::23/64 scope global\n"
           "       valid_lft forever preferred_lft forever\n"
           "    inet6 fe80::1c:2d3e:4f50:6172/64 scope link\n"
           "       valid_lft forever preferred_lft forever\n"
           "5: en1: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 state DOWN group default\n"
           "    link/ether 02:11:22:33:44:55 brd ff:ff:ff:ff:ff:ff\n"
           "7: utun0: <POINTOPOINT,MULTICAST,UP,LOWER_UP> mtu 1380 state UP group default\n"
           "    link/none \n"
           "    inet 10.8.0.2 peer 10.8.0.1/32 scope global utun0\n"
           "       valid_lft forever preferred_lft forever\n",
           "");
    expect(&snap, "address show dev lo0", 0,
           "1: lo: <LOOPBACK,MULTICAST,UP,LOWER_UP> mtu 16384 state UNKNOWN group default\n"
           "    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n"
           "    inet 127.0.0.1/8 scope host lo\n"
           "       valid_lft forever preferred_lft forever\n"
           "    inet6 ::1/128 scope host\n"
           "       valid_lft forever preferred_lft forever\n",
           "");
    expect(&snap, "-4 addr show en0", 0,
           "4: en0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP group default\n"
           "    inet 192.168.1.23/24 brd 192.168.1.255 scope global en0\n"
           "       valid_lft forever preferred_lft forever\n",
           "");
    expect(&snap, "-6 -o a show dev en0", 0,
           "4: en0    inet6 2001:db8::23/64 scope global\\       valid_lft forever preferred_lft forever\n"
           "4: en0    inet6 fe80::1c:2d3e:4f50:6172/64 scope link\\       valid_lft forever preferred_lft forever\n",
           "");
    expect(&snap, "-br a", 0,
           "lo               UNKNOWN        127.0.0.1/8 ::1/128 \n"
           "en0              UP             192.168.1.23/24 2001:db8::23/64 fe80::1c:2d3e:4f50:6172/64 \n"
           "en1              DOWN           \n"
           "utun0            UP             10.8.0.2 peer 10.8.0.1/32 \n",
           "");
    expect(&snap, "-br link", 0,
           "lo               UNKNOWN        00:00:00:00:00:00 <LOOPBACK,MULTICAST,UP,LOWER_UP> \n"
           "en0              UP             02:11:22:33:44:55 <BROADCAST,MULTICAST,UP,LOWER_UP> \n"
           "en1              DOWN           02:11:22:33:44:55 <NO-CARRIER,BROADCAST,MULTICAST,UP> \n"
           "utun0            UP             <POINTOPOINT,MULTICAST,UP,LOWER_UP> \n",
           "");
    expect(&snap, "link show up", 0,
           "1: lo: <LOOPBACK,MULTICAST,UP,LOWER_UP> mtu 16384 state UNKNOWN mode DEFAULT group default\n"
           "    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00\n"
           "4: en0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 state UP mode DEFAULT group default\n"
           "    link/ether 02:11:22:33:44:55 brd ff:ff:ff:ff:ff:ff\n"
           "5: en1: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 state DOWN mode DEFAULT group default\n"
           "    link/ether 02:11:22:33:44:55 brd ff:ff:ff:ff:ff:ff\n"
           "7: utun0: <POINTOPOINT,MULTICAST,UP,LOWER_UP> mtu 1380 state UP mode DEFAULT group default\n"
           "    link/none \n",
           "");
    expect(&snap, "r", 0,
           "default via 192.168.1.1 dev en0\n"
           "10.0.0.0/8 via 10.8.0.1 dev utun0\n"
           "10.8.0.1 dev utun0 proto kernel scope link src 10.8.0.2\n"
           "169.254.0.0/16 dev en0 scope link\n"
           "192.168.1.0/24 dev en0 proto kernel scope link src 192.168.1.23\n",
           "");
    expect(&snap, "route show dev en0 default", 0, "default via 192.168.1.1 dev en0\n", "");
    expect(&snap, "-6 route", 0,
           "default via fe80::1 dev en0 metric 1024 pref medium\n"
           "2001:db8::/64 dev en0 proto kernel metric 256 pref medium\n"
           "fe80::/64 dev en0 proto kernel metric 256 pref medium\n",
           "");
    expect(&snap, "-j route show dev utun0", 0,
           "[{\"dst\":\"10.0.0.0/8\",\"gateway\":\"10.8.0.1\",\"dev\":\"utun0\",\"flags\":[]},"
           "{\"dst\":\"10.8.0.1\",\"dev\":\"utun0\",\"protocol\":\"kernel\",\"scope\":\"link\","
           "\"prefsrc\":\"10.8.0.2\",\"flags\":[]}]\n",
           "");
    expect(&snap, "-j -br a show lo", 0,
           "[{\"ifname\":\"lo\",\"operstate\":\"UNKNOWN\",\"addr_info\":[{\"local\":\"127.0.0.1\",\"prefixlen\":8},"
           "{\"local\":\"::1\",\"prefixlen\":128}]}]\n",
           "");
    expect(&snap, "-j -4 a show en0", 0,
           "[{\"ifindex\":4,\"ifname\":\"en0\",\"flags\":[\"BROADCAST\",\"MULTICAST\",\"UP\",\"LOWER_UP\"],"
           "\"mtu\":1500,\"operstate\":\"UP\",\"group\":\"default\",\"addr_info\":[{\"family\":\"inet\","
           "\"local\":\"192.168.1.23\",\"prefixlen\":24,\"broadcast\":\"192.168.1.255\",\"scope\":\"global\","
           "\"label\":\"en0\",\"valid_life_time\":4294967295,\"preferred_life_time\":4294967295}]}]\n",
           "");
    expect(&snap, "-j -p link show lo", 0,
           "[\n"
           "    {\n"
           "        \"ifindex\": 1,\n"
           "        \"ifname\": \"lo\",\n"
           "        \"flags\": [\n"
           "            \"LOOPBACK\",\n"
           "            \"MULTICAST\",\n"
           "            \"UP\",\n"
           "            \"LOWER_UP\"\n"
           "        ],\n"
           "        \"mtu\": 16384,\n"
           "        \"operstate\": \"UNKNOWN\",\n"
           "        \"linkmode\": \"DEFAULT\",\n"
           "        \"group\": \"default\",\n"
           "        \"link_type\": \"loopback\",\n"
           "        \"address\": \"00:00:00:00:00:00\",\n"
           "        \"broadcast\": \"00:00:00:00:00:00\"\n"
           "    }\n"
           "]\n",
           "");
    {
        int rc = 0;
        char *out = run(&snap, "route get 8.8.8.8", &rc, NULL);
        CHECK(rc == 0 && startsWith(out, "8.8.8.8 via 192.168.1.1 dev en0 src 192.168.1.23 uid "),
              "route get 8.8.8.8: %s", out);
        free(out);
        out = run(&snap, "route get 10.1.2.3", &rc, NULL);
        CHECK(rc == 0 && startsWith(out, "10.1.2.3 via 10.8.0.1 dev utun0 src 10.8.0.2 uid "),
              "route get 10.1.2.3: %s", out);
        free(out);
        out = run(&snap, "route get 192.168.1.23", &rc, NULL);
        CHECK(rc == 0 && startsWith(out, "local 192.168.1.23 dev lo src 192.168.1.23 uid "),
              "route get own address: %s", out);
        free(out);
        out = run(&snap, "-6 route get 2001:4860::8888", &rc, NULL);
        CHECK(rc == 0 && strcmp(out, "2001:4860::8888 from :: via fe80::1 dev en0 src 2001:db8::23 metric 1024 pref medium\n") == 0,
              "-6 route get: %s", out);
        free(out);
    }

    /* Changes: Linux's answer to an unprivileged user, after the device check. */
    expect(&snap, "link set lo down", 2, "", "RTNETLINK answers: Operation not permitted\n");
    expect(&snap, "link set dev en0 mtu 1400", 2, "", "RTNETLINK answers: Operation not permitted\n");
    expect(&snap, "link set nosuch up", 1, "", "Cannot find device \"nosuch\"\n");
    expect(&snap, "addr add 10.0.0.1/24 dev en0", 2, "", "RTNETLINK answers: Operation not permitted\n");
    expect(&snap, "a del 10.0.0.1/24 dev nosuch", 1, "", "Cannot find device \"nosuch\"\n");
    expect(&snap, "addr flush dev lo", 2, "", "RTNETLINK answers: Operation not permitted\n");
    expect(&snap, "route add default via 10.0.0.1", 2, "", "RTNETLINK answers: Operation not permitted\n");
    expect(&snap, "r del 10.0.0.0/8 dev utun0", 2, "", "RTNETLINK answers: Operation not permitted\n");
    expect(&snap, "link add dummy0 type dummy", 2, "", "RTNETLINK answers: Operation not permitted\n");

    /* Errors, in iproute2's words. */
    expect(&snap, "a show dev nosuch", 1, "", "Device \"nosuch\" does not exist.\n");
    expect(&snap, "link show nosuch", 1, "", "Device \"nosuch\" does not exist.\n");
    expect(&snap, "addr frob", 255, "", "Command \"frob\" is unknown, try \"ip address help\".\n");
    expect(&snap, "frob", 255, "", "Object \"frob\" is unknown, try \"ip help\".\n");
    expect(&snap, "-z a", 255, "", "Option \"-z\" is unknown, try \"ip -help\".\n");
    expect(&snap, "route show 1.2.3.4/x", 1, "", "Error: any valid prefix is expected rather than \"1.2.3.4/x\".\n");

    /* /proc/net from the same snapshot. Words of a route are in memory order: on a
     * little-endian machine 192.168.1.0 is 0001A8C0. */
    char *route_text = proc(&snap, pscalNetWriteProcRoute);
    const char *want_route[] = {
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT",
        "en0\t00000000\t0101A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0",
        "utun0\t0000000A\t0100080A\t0003\t0\t0\t0\t000000FF\t0\t0\t0",
        "utun0\t0100080A\t00000000\t0005\t0\t0\t0\tFFFFFFFF\t0\t0\t0",
        "en0\t0000FEA9\t00000000\t0001\t0\t0\t0\t0000FFFF\t0\t0\t0",
        "en0\t0001A8C0\t00000000\t0001\t0\t0\t0\t00FFFFFF\t0\t0\t0",
    };
    char *cursor = route_text;
    for (size_t i = 0; i < sizeof(want_route) / sizeof(want_route[0]); ++i) {
        char *nl = strchr(cursor, '\n');
        CHECK(nl != NULL, "/proc/net/route ends early at row %zu", i);
        if (!nl) {
            break;
        }
        CHECK(nl - cursor == 127, "/proc/net/route row %zu is %td wide", i, nl - cursor);
        size_t want_len = strlen(want_route[i]);
        CHECK(strncmp(cursor, want_route[i], want_len) == 0, "/proc/net/route row %zu: %.*s", i,
              (int)want_len, cursor);
        cursor = nl + 1;
    }
    CHECK(*cursor == '\0', "/proc/net/route has more: %s", cursor);
    free(route_text);

    char *inet6 = proc(&snap, pscalNetWriteProcIfInet6);
    CHECK(strcmp(inet6,
                 "00000000000000000000000000000001 01 80 10 80       lo\n"
                 "20010db8000000000000000000000023 04 40 00 80      en0\n"
                 "fe80000000000000001c2d3e4f506172 04 40 20 80      en0\n") == 0,
          "/proc/net/if_inet6:\n%s", inet6);
    free(inet6);

    char *v6route = proc(&snap, pscalNetWriteProcIpv6Route);
    CHECK(strstr(v6route, "00000000000000000000000000000000 00 00000000000000000000000000000000 00 "
                          "fe800000000000000000000000000001 00000400 00000001 00000000 00000003      en0\n") == v6route,
          "/proc/net/ipv6_route:\n%s", v6route);
    free(v6route);

    char *dev = proc(&snap, pscalNetWriteProcDev);
    CHECK(strstr(dev, "    lo:    1000      10    0    0    0     0          0         0     2000      20    0    0    0     0       0          0\n") != NULL,
          "/proc/net/dev:\n%s", dev);
    CHECK(strstr(dev, "   en0:    4000      40 ") != NULL, "/proc/net/dev en0:\n%s", dev);
    free(dev);

    pscalNetSnapshotFree(&snap);
}

static void testLive(void) {
    PscalNetSnapshot snap;
    CHECK(pscalNetSnapshotTake(&snap), "live snapshot");
    const PscalNetLink *lo = pscalNetSnapshotFindLink(&snap, "lo");
    CHECK(lo != NULL, "live: no lo");
    int rc = 0;
    char *out = run(&snap, "a show lo", &rc, NULL);
    CHECK(rc == 0 && strstr(out, "inet 127.0.0.1/8 scope host lo\n") != NULL, "live ip a show lo:\n%s", out);
    free(out);
    out = run(&snap, "route", &rc, NULL);
    CHECK(rc == 0, "live ip route: %d", rc);
    free(out);
    pscalNetSnapshotFree(&snap);
}

int main(void) {
    testMadeUp();
    testLive();
    if (failures) {
        fprintf(stderr, "test_ip_app: %d failed\n", failures);
        return 1;
    }
    printf("test_ip_app: ok\n");
    return 0;
}
