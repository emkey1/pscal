#pragma once

#if defined(VPROC_SHIM_DISABLED) && !defined(VPROC_SHIM_HARD_DISABLED)
#undef VPROC_SHIM_DISABLED
#endif

#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#include <sys/stat.h>
#define VPROC_SHIM_PRESENT 1
#include "ios/vproc.h"

/* Some third-party components (e.g., OpenSSH) provide their own iOS shims.
 * If their shim header is present, do not redefine the syscall macros here. */
#if !defined(PSCAL_IOS_SHIM_H)

#undef read
#undef write
#undef dup
#undef dup2
#undef close
#undef pipe
#undef fstat
#undef stat
#undef lstat
#undef ioctl
#undef lseek
#undef isatty
#undef poll
#undef select
#undef waitpid
#undef kill
#undef getpid
#undef getppid
#undef getpgrp
#undef getpgid
#undef setpgid
#undef getsid
#undef setsid
#undef tcgetpgrp
#undef tcsetpgrp
#undef pthread_create
#undef sigaction
#undef sigprocmask
#undef sigpending
#undef sigsuspend
#undef signal
#undef raise
#undef pthread_sigmask

#define read  vprocReadShim
#define write vprocWriteShim
#define dup   vprocDupShim
#define dup2  vprocDup2Shim
#define close vprocCloseShim
#define pipe  vprocPipeShim
#define fstat vprocFstatShim
#define stat(path, buf) vprocStatShim((path), (buf))
#define lstat(path, buf) vprocLstatShim((path), (buf))
#define ioctl vprocIoctlShim
#define lseek vprocLseekShim
#define isatty vprocIsattyShim
#define poll vprocPollShim
#define select vprocSelectShim
#ifndef open
#define open  vprocOpenShim
#endif
#define waitpid vprocWaitPidShim
#define kill  vprocKillShim
#define getpid vprocGetPidShim
#define getppid vprocGetPpidShim
#define getpgrp vprocGetpgrpShim
#define getpgid vprocGetpgidShim
#define setpgid vprocSetpgidShim
#define getsid vprocGetsidShim
#define setsid vprocSetsidShim
#define tcgetpgrp vprocTcgetpgrpShim
#define tcsetpgrp vprocTcsetpgrpShim
#define pthread_create vprocPthreadCreateShim
#define sigaction(sig, act, oldact) vprocSigactionShim((sig), (act), (oldact))
#define sigprocmask(how, set, oldset) vprocSigprocmaskShim((how), (set), (oldset))
#define sigpending(set) vprocSigpendingShim((set))
#define sigsuspend(mask) vprocSigsuspendShim((mask))
#define signal(sig, handler) vprocSignalShim((sig), (handler))
#define raise(sig) vprocRaiseShim((sig))
#define pthread_sigmask(how, set, oldset) vprocPthreadSigmaskShim((how), (set), (oldset))

#endif /* !PSCAL_IOS_SHIM_H */

#endif /* PSCAL_TARGET_IOS */
