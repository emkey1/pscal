#ifndef PSCAL_HOSTS_H
#define PSCAL_HOSTS_H

#include <netdb.h>
#include <stdbool.h>

#ifndef PSCAL_HOSTS_CUSTOM_IMPL
/* Portable wrapper that consults the iOS container hosts file when available. */
int pscalHostsGetAddrInfo(const char *node, const char *service,
                          const struct addrinfo *hints, struct addrinfo **res);
void pscalHostsFreeAddrInfo(struct addrinfo *ai);
#endif
const char *pscalHostsGetContainerPath(void);
bool pscalHostsLogEnabled(void);
void pscalHostsSetLogEnabled(int enabled); /* -1 auto/env, 0 off, 1 on */

#endif /* PSCAL_HOSTS_H */
