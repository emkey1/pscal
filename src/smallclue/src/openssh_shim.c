#include <sys/socket.h>
#include <stdint.h>
#include <stdio.h>
#include "third-party/ios_system/ssh_keygen/sshkey.h"

/* Stub DNS host key verification: DNS SSHFP lookups disabled in this build. */
int verify_host_key_dns(const char *hostname, struct sockaddr *address,
                        struct sshkey *hostkey, int *flags) {
    (void)hostname;
    (void)address;
    (void)hostkey;
    if (flags) {
        *flags = 0;
    }
    return -1; /* act as if no DNS data was found */
}
