#pragma once

#include <stdbool.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pscalRuntimeFdIsInteractive(int fd);
bool pscalRuntimeStdinIsInteractive(void);
bool pscalRuntimeStdoutIsInteractive(void);
bool pscalRuntimeStderrIsInteractive(void);

bool pscalRuntimeFdHasRealTTY(int fd);
bool pscalRuntimeStdinHasRealTTY(void);
bool pscalRuntimeStdoutHasRealTTY(void);
bool pscalRuntimeStderrHasRealTTY(void);

int pscalRuntimeDetectWindowRows(void);
int pscalRuntimeDetectWindowCols(void);

#ifdef __cplusplus
}
#endif
