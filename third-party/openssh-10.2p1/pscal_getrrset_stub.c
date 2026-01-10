/*
 * PSCAL iOS resolver stub
 *
 * iOS does not ship a static libresolv, so the OpenBSD getrrsetbyname()
 * compatibility layer cannot link.  For the iOS build we disable DNS-based
 * host key verification altogether by stubbing out getrrsetbyname() so the
 * rest of the client can link without pulling in libresolv.
 */

#include "includes.h"
#include "openbsd-compat/getrrsetbyname.h"

int
getrrsetbyname(const char *hostname, unsigned int rdclass,
    unsigned int rdtype, unsigned int flags, struct rrsetinfo **result)
{
	/* DNS-based host key verification is not supported on iOS. */
	if (result != NULL)
		*result = NULL;
	errno = ENOSYS;
	return ERRSET_FAIL;
}

void
freerrset(struct rrsetinfo *rrset)
{
	/* Nothing to free because getrrsetbyname() never allocates. */
	(void)rrset;
}
