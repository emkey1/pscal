/* The app's ping packets (ios/Sources/Bridge/PSCALPingPacket.h): the request's
 * checksum, the reply decoder on Darwin's layout (IPv4 header in front) and on
 * a bare ICMP message, and on Darwin a real round trip to 127.0.0.1 through a
 * SOCK_DGRAM ICMP socket, the one the app opens. Before the checksum was
 * stored in network order that round trip timed out, as every ping in the app
 * did. */
#include "ios/Sources/Bridge/PSCALPingPacket.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, what)                                   \
    do {                                                    \
        if (!(cond)) {                                      \
            fprintf(stderr, "FAIL %s (line %d)\n", what, __LINE__); \
            failures++;                                     \
        }                                                   \
    } while (0)

enum { PAYLOAD = 56 };

static size_t buildRequest(unsigned char *packet, uint16_t ident, uint16_t seq, uint64_t token) {
    memset(packet, 0, ICMP_MINLEN + PAYLOAD);
    struct icmp *h = (struct icmp *)packet;
    h->icmp_type = ICMP_ECHO;
    h->icmp_id = htons(ident);
    h->icmp_seq = htons(seq);
    memcpy(packet + ICMP_MINLEN, &token, sizeof(token));
    for (size_t i = sizeof(token); i < PAYLOAD; ++i) {
        packet[ICMP_MINLEN + i] = (unsigned char)(i & 0xffu);
    }
    h->icmp_cksum = 0;
    h->icmp_cksum = PSCALPingChecksum(packet, ICMP_MINLEN + PAYLOAD);
    return ICMP_MINLEN + PAYLOAD;
}

/* RFC 1071 as a receiver checks it: the ones' complement sum of the whole
 * message, checksum included, read in network order, is 0xffff. */
static int receiverAccepts(const unsigned char *p, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += (uint32_t)((p[i] << 8) | p[i + 1]);
    }
    if (len & 1) {
        sum += (uint32_t)(p[len - 1] << 8);
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return sum == 0xffffu;
}

/* What a Darwin ping socket hands back: a 20-byte IPv4 header, then the echo
 * reply, which is the request with type 0 and its checksum redone. */
static size_t darwinReply(const unsigned char *request, size_t len, unsigned char *out) {
    memset(out, 0, 20);
    struct ip *ip = (struct ip *)out;
    ip->ip_v = 4;
    ip->ip_hl = 5;
    ip->ip_p = IPPROTO_ICMP;
    memcpy(out + 20, request, len);
    struct icmp *h = (struct icmp *)(out + 20);
    h->icmp_type = ICMP_ECHOREPLY;
    h->icmp_cksum = 0;
    h->icmp_cksum = PSCALPingChecksum(out + 20, len);
    return 20 + len;
}

static void testChecksum(void) {
    unsigned char packet[ICMP_MINLEN + PAYLOAD];
    bool all = true;
    for (uint16_t seq = 1; seq < 300 && all; ++seq) {
        size_t len = buildRequest(packet, 0x1234, seq, 0x0102030405060708ull * seq);
        all = receiverAccepts(packet, len);
    }
    CHECK(all, "request checksum verifies in network order");
    /* RFC 1071's own example bytes: 00 01 f2 03 f4 f5 f6 f7 sum to ddf2, so
     * the checksum on the wire is 22 0d. */
    const unsigned char rfc[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    uint16_t ck = PSCALPingChecksum(rfc, sizeof(rfc));
    const unsigned char *wire = (const unsigned char *)&ck;
    CHECK(wire[0] == 0x22 && wire[1] == 0x0d, "RFC 1071 example, byte order on the wire");
    /* An odd length pads with a zero byte. */
    const unsigned char odd[] = {0x45, 0x00, 0x01};
    ck = PSCALPingChecksum(odd, sizeof(odd));
    wire = (const unsigned char *)&ck;
    CHECK(wire[0] == 0xb9 && wire[1] == 0xff, "odd length");
}

static void testDecode(void) {
    unsigned char packet[ICMP_MINLEN + PAYLOAD];
    unsigned char reply[20 + ICMP_MINLEN + PAYLOAD];
    uint64_t token = 0xfeedfacecafebeefull;
    size_t len = buildRequest(packet, 0x4242, 7, token);
    size_t rlen = darwinReply(packet, len, reply);
    size_t got = 0;

    CHECK(PSCALPingDecodeReply(reply, rlen, AF_INET, 7, token, &got), "Darwin layout accepted");
    CHECK(got == ICMP_MINLEN + PAYLOAD, "length is the ICMP message's (64), not the datagram's");

    got = 0;
    CHECK(PSCALPingDecodeReply(reply + 20, rlen - 20, AF_INET, 7, token, &got), "bare ICMP accepted");
    CHECK(got == ICMP_MINLEN + PAYLOAD, "bare ICMP length");

    CHECK(!PSCALPingDecodeReply(reply, rlen, AF_INET, 8, token, NULL), "other seq refused");
    CHECK(!PSCALPingDecodeReply(reply, rlen, AF_INET, 7, token + 1, NULL), "other token refused");
    CHECK(!PSCALPingDecodeReply(packet, len, AF_INET, 7, token, NULL), "our own request refused");
    CHECK(!PSCALPingDecodeReply(reply, 10, AF_INET, 7, token, NULL), "short datagram refused");
    CHECK(!PSCALPingDecodeReply(reply, rlen, AF_UNIX, 7, token, NULL), "other family refused");

    /* IPv6 ping sockets give the bare ICMPv6 message. */
    unsigned char r6[sizeof(struct icmp6_hdr) + PAYLOAD];
    memset(r6, 0, sizeof(r6));
    struct icmp6_hdr *h6 = (struct icmp6_hdr *)r6;
    h6->icmp6_type = ICMP6_ECHO_REPLY;
    h6->icmp6_seq = htons(3);
    memcpy(r6 + sizeof(*h6), &token, sizeof(token));
    got = 0;
    CHECK(PSCALPingDecodeReply(r6, sizeof(r6), AF_INET6, 3, token, &got), "ICMPv6 reply accepted");
    CHECK(got == sizeof(r6), "ICMPv6 length");
    h6->icmp6_type = ICMP6_ECHO_REQUEST;
    CHECK(!PSCALPingDecodeReply(r6, sizeof(r6), AF_INET6, 3, token, NULL), "ICMPv6 request refused");
}

static void testLoopback(void) {
#if defined(__APPLE__)
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s < 0) {
        printf("SKIP loopback: socket: %s\n", strerror(errno));
        return;
    }
    struct timeval tv = {2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    uint64_t token = ((uint64_t)getpid() << 32) | 0x5a5au;
    unsigned char packet[ICMP_MINLEN + PAYLOAD];
    size_t len = buildRequest(packet, (uint16_t)getpid(), 1, token);
    ssize_t sent = sendto(s, packet, len, 0, (struct sockaddr *)&to, sizeof(to));
    CHECK(sent == (ssize_t)len, "loopback send");

    bool matched = false;
    for (int tries = 0; tries < 8 && !matched; ++tries) {
        unsigned char buf[1500];
        ssize_t n = recv(s, buf, sizeof(buf), 0);
        if (n < 0) {
            fprintf(stderr, "loopback recv: %s\n", strerror(errno));
            break;
        }
        size_t got = 0;
        matched = PSCALPingDecodeReply(buf, (size_t)n, AF_INET, 1, token, &got);
        if (matched) {
            CHECK(got == len, "loopback reply length");
        }
    }
    CHECK(matched, "loopback echo reply received and recognised");
    close(s);
#endif
}

int main(void) {
    testChecksum();
    testDecode();
    testLoopback();
    if (failures) {
        fprintf(stderr, "test_ping_packet: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_ping_packet: ok\n");
    return 0;
}
