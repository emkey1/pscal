#ifndef SMALLCLUE_PSCAL_HOSTS_H
#define SMALLCLUE_PSCAL_HOSTS_H

#include <netdb.h>

/* Lightweight wrappers around getaddrinfo/freeaddrinfo. */
void pscalHostsSetLogEnabled(int enabled);
int pscalHostsGetAddrInfo(const char *node,
                          const char *service,
                          const struct addrinfo *hints,
                          struct addrinfo **res);
void pscalHostsFreeAddrInfo(struct addrinfo *res);
const char *pscalHostsGetContainerPath(void);

#endif /* SMALLCLUE_PSCAL_HOSTS_H */
