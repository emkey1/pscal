#pragma once

#include "ios/tty/pscal_tty.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pscal_fd;

int pscalPtyOpenMaster(int flags, struct pscal_fd **out_master, int *out_pty_num);
int pscalPtyOpenSlave(int pty_num, int flags, struct pscal_fd **out_slave);
int pscalPtyUnlock(struct pscal_fd *master);

bool pscalPtyIsMaster(struct pscal_fd *fd);
bool pscalPtyIsSlave(struct pscal_fd *fd);
int pscalPtyGetSlaveInfo(int pty_num, mode_t_ *perms, uid_t_ *uid, gid_t_ *gid);

#ifdef __cplusplus
}
#endif
