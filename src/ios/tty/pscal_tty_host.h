#pragma once

#include "ios/tty/ish_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tty;

int pscalTtyCurrentPid(void);
int pscalTtyCurrentPgid(void);
int pscalTtyCurrentSid(void);
bool pscalTtyIsSessionLeader(void);
bool pscalTtyIsControlling(struct tty *tty);
void pscalTtySetControlling(struct tty *tty);
void pscalTtyClearControlling(struct tty *tty);
void pscalTtySetForegroundPgid(int sid, int fg_pgid);
int pscalTtySendGroupSignal(int pgid, int sig);

#ifdef __cplusplus
}
#endif
