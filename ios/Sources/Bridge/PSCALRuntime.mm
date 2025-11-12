#include "PSCALRuntime.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if __has_include(<util.h>)
#include <util.h>
#else
#include <pty.h>
#endif

#if __has_include(<sanitizer/asan_interface.h>)
#define PSCAL_HAS_ASAN_INTERFACE 1
#include <sanitizer/asan_interface.h>
#else
#define PSCAL_HAS_ASAN_INTERFACE 0
#endif

extern "C" {
    // Forward declare exsh entrypoint exposed by the existing CLI target.
    int exsh_main(int argc, char* argv[]);
}

static PSCALRuntimeOutputHandler s_output_handler = NULL;
static PSCALRuntimeExitHandler s_exit_handler = NULL;
static void *s_handler_context = NULL;

static pthread_mutex_t s_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_runtime_active = false;
static int s_master_fd = -1;
static pthread_t s_output_thread;

void PSCALRuntimeConfigureHandlers(PSCALRuntimeOutputHandler output_handler,
                                   PSCALRuntimeExitHandler exit_handler,
                                   void *context) {
    pthread_mutex_lock(&s_runtime_mutex);
    s_output_handler = output_handler;
    s_exit_handler = exit_handler;
    s_handler_context = context;
    pthread_mutex_unlock(&s_runtime_mutex);
}

static void PSCALRuntimeDispatchOutput(const char *buffer, size_t length) {
    pthread_mutex_lock(&s_runtime_mutex);
    PSCALRuntimeOutputHandler handler = s_output_handler;
    void *context = s_handler_context;
    pthread_mutex_unlock(&s_runtime_mutex);

    if (handler && buffer && length > 0) {
        char *heap_buffer = (char *)malloc(length);
        if (!heap_buffer) {
            return;
        }
        memcpy(heap_buffer, buffer, length);
        handler(heap_buffer, length, context);
    }
}

static void *PSCALRuntimeOutputPump(void *_) {
    (void)_;
    const int fd = s_master_fd;
    char buffer[4096];
    while (true) {
        ssize_t nread = read(fd, buffer, sizeof(buffer));
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (nread == 0) {
            break; // EOF
        }
        PSCALRuntimeDispatchOutput(buffer, (size_t)nread);
    }
    return NULL;
}

int PSCALRuntimeLaunchExsh(int argc, char* argv[]) {
    pthread_mutex_lock(&s_runtime_mutex);
    if (s_runtime_active) {
        pthread_mutex_unlock(&s_runtime_mutex);
        errno = EBUSY;
        return -1;
    }

    int master_fd = -1;
    int slave_fd = -1;
    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) != 0) {
        pthread_mutex_unlock(&s_runtime_mutex);
        return -1;
    }

    s_master_fd = master_fd;
    s_runtime_active = true;
    pthread_mutex_unlock(&s_runtime_mutex);

    // Ensure stdio is line-buffered at most to reduce latency.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    // Redirect stdio to the PTY slave so exsh sees a real terminal.
    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(slave_fd, STDERR_FILENO);
    close(slave_fd);

    pthread_create(&s_output_thread, NULL, PSCALRuntimeOutputPump, NULL);

    int result = exsh_main(argc, argv);

    // Tear down PTY + output pump.
    pthread_mutex_lock(&s_runtime_mutex);
    int pump_fd = s_master_fd;
    s_master_fd = -1;
    s_runtime_active = false;
    pthread_mutex_unlock(&s_runtime_mutex);

    if (pump_fd >= 0) {
        close(pump_fd);
    }
    pthread_join(s_output_thread, NULL);

    pthread_mutex_lock(&s_runtime_mutex);
    PSCALRuntimeExitHandler exit_handler = s_exit_handler;
    void *context = s_handler_context;
    pthread_mutex_unlock(&s_runtime_mutex);

    if (exit_handler) {
        exit_handler(result, context);
    }

    return result;
}

void PSCALRuntimeSendInput(const char *utf8, size_t length) {
    if (!utf8 || length == 0) {
        return;
    }
    pthread_mutex_lock(&s_runtime_mutex);
    const int fd = s_master_fd;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (fd < 0) {
        return;
    }
    size_t written = 0;
    while (written < length) {
        ssize_t chunk = write(fd, utf8 + written, length - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        written += (size_t)chunk;
    }
}

int PSCALRuntimeIsRunning(void) {
    pthread_mutex_lock(&s_runtime_mutex);
    const bool active = s_runtime_active;
    pthread_mutex_unlock(&s_runtime_mutex);
    return active ? 1 : 0;
}

void PSCALRuntimeConfigureAsanReportPath(const char *path) {
#if PSCAL_HAS_ASAN_INTERFACE
    if (path && *path) {
        __sanitizer_set_report_path(path);
    }
#else
    (void)path;
#endif
}
