#pragma once

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
#undef lseek
#undef waitpid
#undef kill
#undef getpid

#define read  vprocReadShim
#define write vprocWriteShim
#define dup   vprocDupShim
#define dup2  vprocDup2Shim
#define close vprocCloseShim
#define pipe  vprocPipeShim
#define fstat vprocFstatShim
#define lseek vprocLseekShim
#ifndef open
#define open  vprocOpenShim
#endif
#define waitpid vprocWaitPidShim
#define kill  vprocKillShim
#define getpid vprocGetPidShim

#endif /* !PSCAL_IOS_SHIM_H */

#endif /* PSCAL_TARGET_IOS */
