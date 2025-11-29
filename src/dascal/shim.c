#include <stdlib.h>

/*
 * Thin shim used by the iOS/iPadOS host to expose the dascal entry point
 * without linking a second copy of the compiler into the app bundle. The
 * desktop build still produces a full debug binary via BUILD_DASCAL, but on
 * iOS we simply forward to the existing pascal_main implementation and mark
 * the invocation so future runtime toggles can differentiate dascal sessions.
 */

extern int pascal_main(int argc, char *argv[]);

int dascal_main(int argc, char *argv[]) {
    setenv("PSCAL_DASCAL_MODE", "1", 1);
    return pascal_main(argc, argv);
}
