#include <stddef.h>

/*
 * Provide no-op implementations of shell runtime status helpers when the shell
 * front end (which normally defines them) is not part of the build.  Clang on
 * macOS does not treat the weak-import declarations in builtin.c as optional
 * during static linking, so these fallbacks satisfy the linker while keeping
 * the behaviour consistent with other platforms (the helpers simply do
 * nothing).
 */

#if !defined(FRONTEND_SHELL)

#  if defined(__has_attribute)
#    if __has_attribute(weak)
#      define PSCAL_WEAK __attribute__((weak))
#    else
#      define PSCAL_WEAK
#    endif
#  else
#    define PSCAL_WEAK
#  endif

PSCAL_WEAK void shellRuntimeSetLastStatus(int status) {
    (void)status;
}

PSCAL_WEAK void shellRuntimeSetLastStatusSticky(int status) {
    shellRuntimeSetLastStatus(status);
}

#endif /* !defined(FRONTEND_SHELL) */

