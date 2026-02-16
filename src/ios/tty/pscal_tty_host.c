#include "ios/tty/pscal_tty_host.h"

#if defined(PSCAL_TARGET_IOS)
#define PSCAL_WEAK __attribute__((weak))
#else
#define PSCAL_WEAK
#endif

PSCAL_WEAK int pscalTtyCurrentPid(void) {
    return -1;
}

PSCAL_WEAK int pscalTtyCurrentPgid(void) {
    return -1;
}

PSCAL_WEAK int pscalTtyCurrentSid(void) {
    return -1;
}

PSCAL_WEAK bool pscalTtyIsSessionLeader(void) {
    return false;
}

PSCAL_WEAK bool pscalTtyIsControlling(struct tty *tty) {
    (void)tty;
    return true;
}

PSCAL_WEAK void pscalTtySetControlling(struct tty *tty) {
    (void)tty;
}

PSCAL_WEAK void pscalTtyClearControlling(struct tty *tty) {
    (void)tty;
}

PSCAL_WEAK void pscalTtySetForegroundPgid(int sid, int fg_pgid) {
    (void)sid;
    (void)fg_pgid;
}

PSCAL_WEAK int pscalTtyGetForegroundPgid(int sid) {
    (void)sid;
    return -1;
}

PSCAL_WEAK int pscalTtySendGroupSignal(int pgid, int sig) {
    (void)pgid;
    (void)sig;
    return 0;
}
