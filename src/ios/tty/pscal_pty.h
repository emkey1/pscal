#pragma once

#include "ios/tty/pscal_tty.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pscal_fd;
struct tty_driver;

extern struct tty_driver pty_master;
extern struct tty_driver pty_slave;

int pscalPtyOpenMaster(int flags, struct pscal_fd **out_master, int *out_pty_num);
int pscalPtyOpenSlave(int pty_num, int flags, struct pscal_fd **out_slave);
int pscalPtyUnlock(struct pscal_fd *master);
struct tty *pscalPtyOpenFake(struct tty_driver *driver);

bool pscalPtyIsMaster(struct pscal_fd *fd);
bool pscalPtyIsSlave(struct pscal_fd *fd);
bool pscalPtyExists(int pty_num);
int pscalPtyGetLimit(void);
int pscalPtyGetSlaveInfo(int pty_num, mode_t_ *perms, uid_t_ *uid, gid_t_ *gid);
int pscalPtySetSlaveInfo(int pty_num, const mode_t_ *perms, const uid_t_ *uid, const gid_t_ *gid);

#ifdef __cplusplus
}
#endif
