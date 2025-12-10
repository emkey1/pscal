#ifndef SMALLCLUE_RUNTIME_TTY_H
#define SMALLCLUE_RUNTIME_TTY_H

#include <stdbool.h>

/* Minimal terminal helpers used by smallclue.
 *
 * The real project wires these into platform-specific runtime hooks.
 * These stubs provide sensible defaults so the standalone build works.
 */
bool pscalRuntimeStdoutIsInteractive(void);
bool pscalRuntimeStdinIsInteractive(void);
bool pscalRuntimeFdIsInteractive(int fd);
bool pscalRuntimeStdinHasRealTTY(void);
int pscalRuntimeDetectWindowCols(void);
int pscalRuntimeDetectWindowRows(void);
bool pscalRuntimeConsumeSigint(void);
void pscalRuntimeDebugLog(const char *message);

#endif /* SMALLCLUE_RUNTIME_TTY_H */
