/* ICMP echo packets for the app's ping (PSCALRuntimePingHost), in plain C so a
 * host test (Tests/ios_vproc/test_ping_packet.c) builds them too.
 *
 * Darwin's "ping socket" (SOCK_DGRAM + IPPROTO_ICMP) sends the ICMP message as
 * given: it neither fills in nor fixes the checksum, and it keeps the
 * identifier. A reply comes back with its IPv4 header in front. */
#ifndef PSCAL_PING_PACKET_H
#define PSCAL_PING_PACKET_H

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

/* The checksum to store in icmp_cksum. The loop reads each 16-bit word
 * big-endian, so the sum comes out in host order, and icmp_cksum is a
 * network-order field: hence htons. Without it the two bytes went out swapped
 * on every iPhone and iPad, the far end (loopback included) dropped each
 * request as corrupt, and every ping timed out with EAGAIN. */
static inline uint16_t PSCALPingChecksum(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)((bytes[0] << 8) | bytes[1]);
        bytes += 2;
        len -= 2;
    }
    if (len > 0) {
        sum += (uint32_t)(bytes[0] << 8);
    }
    while ((sum >> 16) != 0) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return htons((uint16_t)~sum);
}

/* An echo reply to the request with this seq and payload token, with or
 * without the IP header in front. On a match *out_reply_len is the length of
 * the ICMP message alone. */
static inline bool PSCALPingDecodeReply(const unsigned char *buf, size_t len,
                                        sa_family_t family,
                                        uint16_t seq,
                                        uint64_t payload_token,
                                        size_t *out_reply_len) {
    size_t reply_len = len;
    const unsigned char *payload = NULL;

    if (family == AF_INET) {
        const struct icmp *icmp_hdr = NULL;
        if (len >= sizeof(struct ip)) {
            const struct ip *ip_hdr = (const struct ip *)buf;
            if (ip_hdr->ip_v == 4) {
                size_t ip_len = (size_t)ip_hdr->ip_hl << 2;
                if (ip_len >= sizeof(struct ip) && len >= ip_len + ICMP_MINLEN) {
                    icmp_hdr = (const struct icmp *)(buf + ip_len);
                    reply_len = len - ip_len;
                }
            }
        }
        if (!icmp_hdr && len >= ICMP_MINLEN) {
            icmp_hdr = (const struct icmp *)buf;
        }
        if (!icmp_hdr) {
            errno = EPROTO;
            return false;
        }
        if (icmp_hdr->icmp_type != ICMP_ECHOREPLY ||
            ntohs((uint16_t)icmp_hdr->icmp_seq) != seq) {
            errno = EPROTO;
            return false;
        }
        if (reply_len < ICMP_MINLEN + sizeof(payload_token)) {
            errno = EPROTO;
            return false;
        }
        payload = ((const unsigned char *)icmp_hdr) + ICMP_MINLEN;
    } else if (family == AF_INET6) {
        const struct icmp6_hdr *icmp6_hdr = NULL;
        if (len >= sizeof(struct ip6_hdr)) {
            const struct ip6_hdr *ip6_hdr = (const struct ip6_hdr *)buf;
            if ((ip6_hdr->ip6_vfc >> 4) == 6 &&
                len >= sizeof(struct ip6_hdr) + sizeof(struct icmp6_hdr)) {
                icmp6_hdr = (const struct icmp6_hdr *)(buf + sizeof(struct ip6_hdr));
                reply_len = len - sizeof(struct ip6_hdr);
            }
        }
        if (!icmp6_hdr && len >= sizeof(struct icmp6_hdr)) {
            icmp6_hdr = (const struct icmp6_hdr *)buf;
        }
        if (!icmp6_hdr) {
            errno = EPROTO;
            return false;
        }
        if (icmp6_hdr->icmp6_type != ICMP6_ECHO_REPLY ||
            ntohs(icmp6_hdr->icmp6_seq) != seq) {
            errno = EPROTO;
            return false;
        }
        if (reply_len < sizeof(struct icmp6_hdr) + sizeof(payload_token)) {
            errno = EPROTO;
            return false;
        }
        payload = ((const unsigned char *)icmp6_hdr) + sizeof(struct icmp6_hdr);
    } else {
        errno = EPROTO;
        return false;
    }

    uint64_t echoed_token = 0;
    memcpy(&echoed_token, payload, sizeof(echoed_token));
    if (echoed_token != payload_token) {
        errno = EPROTO;
        return false;
    }
    if (out_reply_len) {
        *out_reply_len = reply_len;
    }
    return true;
}

#endif /* PSCAL_PING_PACKET_H */
