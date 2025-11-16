#include "PSCALRuntime.h"

#include <Foundation/Foundation.h>
#include <errno.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <string>
#include <termios.h>
#if __has_include(<util.h>)
#include <util.h>
#else
#include <pty.h>
#endif

#if defined(PSCAL_TARGET_IOS)
#define PSCAL_HAS_ASAN_INTERFACE 0
#elif __has_include(<sanitizer/asan_interface.h>) && __has_feature(address_sanitizer)
#define PSCAL_HAS_ASAN_INTERFACE 1
#include <sanitizer/asan_interface.h>
#else
#define PSCAL_HAS_ASAN_INTERFACE 0
#endif

extern "C" {
    // Forward declare exsh entrypoint exposed by the existing CLI target.
    int exsh_main(int argc, char* argv[]);
    void pscalRuntimeSetVirtualTTYEnabled(bool enabled);
    bool pscalRuntimeVirtualTTYEnabled(void);
    void pscalRuntimeRegisterVirtualTTYFd(int std_fd, int fd);
}

static PSCALRuntimeOutputHandler s_output_handler = NULL;
static PSCALRuntimeExitHandler s_exit_handler = NULL;
static void *s_handler_context = NULL;

static pthread_mutex_t s_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_runtime_active = false;
static int s_master_fd = -1;
static int s_input_fd = -1;
static bool s_using_virtual_tty = false;
static pthread_t s_output_thread;
static pthread_t s_runtime_thread;
static int s_pending_columns = 80;
static int s_pending_rows = 24;
static NSFileHandle *s_debug_log_handle = nil;

static void PSCALRuntimeConfigurePtySlave(int fd) {
    if (fd < 0) {
        return;
    }
    struct termios term;
    if (tcgetattr(fd, &term) != 0) {
        return;
    }
    term.c_lflag |= (ICANON | ECHO | ECHOE | ECHOK | ECHONL);
#ifdef ECHOCTL
    term.c_lflag &= ~ECHOCTL;
#endif
#ifdef ECHOKE
    term.c_lflag &= ~ECHOKE;
#endif
    term.c_iflag |= (ICRNL | IXON);
#ifdef IUTF8
    term.c_iflag |= IUTF8;
#endif
    term.c_oflag |= (OPOST | ONLCR);
    term.c_cflag |= (CS8 | CREAD);
    term.c_cc[VMIN] = 1;
    term.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &term);
}

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
/***********/
static void PSCALRuntimeEnsureDebugLog(void) {
    if (s_debug_log_handle) {
        return;
    }
    NSArray<NSURL *> *dirs = [[NSFileManager defaultManager] URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask];
    NSURL *doc = dirs.firstObject;
    if (!doc) {
        return;
    }
    NSURL *logURL = [doc URLByAppendingPathComponent:@"elvis_debug.log"];
    [[NSFileManager defaultManager] createFileAtPath:logURL.path contents:nil attributes:nil];
    NSError *error = nil;
    NSFileHandle *handle = [NSFileHandle fileHandleForWritingToURL:logURL error:&error];
    if (!handle) {
        NSLog(@"PSCALRuntime: failed to open debug log: %@", error.localizedDescription);
        return;
    }
    [handle seekToEndOfFile];
    s_debug_log_handle = handle;
}

void pscalRuntimeDebugLog(const char *message) {
    if (!message) return;
    NSString *line = [NSString stringWithUTF8String:message];
    if (!line) {
        line = @"(log conversion failure)";
    }
    NSLog(@"%@", line);
    PSCALRuntimeEnsureDebugLog();
    if (s_debug_log_handle) {
        NSData *data = [[line stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
        [s_debug_log_handle writeData:data];
        [s_debug_log_handle synchronizeFile];
    }
}

static bool PSCALRuntimeShouldSuppressLogLine(const std::string &line) {
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
        trimmed.pop_back();
    }
    static const char *kSuppressedFragments[] = {
        "TextInputActionsAnalytics",
        "inputOperation = dismissAutoFillPanel",
        "PSCALRuntime: using virtual terminal pipes",
        "OSLOG",
        "Falling back to first defined description for UIWindowSceneSessionRoleApplication"
    };
    for (size_t i = 0; i < sizeof(kSuppressedFragments) / sizeof(kSuppressedFragments[0]); ++i) {
        if (trimmed.find(kSuppressedFragments[i]) != std::string::npos) {
            return true;
        }
    }
    return false;
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
        std::string chunk(buffer, (size_t)nread);
        if (PSCALRuntimeShouldSuppressLogLine(chunk)) {
            continue;
        }
        PSCALRuntimeDispatchOutput(buffer, (size_t)nread);
    }
    return NULL;
}

static void PSCALRuntimeApplyWindowSize(int fd, int columns, int rows) {
    if (fd < 0 || columns <= 0 || rows <= 0) {
        return;
    }
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)columns;
    ws.ws_row = (unsigned short)rows;
    ioctl(fd, TIOCSWINSZ, &ws);
}

static bool PSCALRuntimeInstallVirtualTTY(int *out_master_fd, int *out_input_fd) {
    if (!out_master_fd || !out_input_fd) {
        return false;
    }

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0) {
        return false;
    }
    if (pipe(stdout_pipe) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return false;
    }

    if (dup2(stdin_pipe[0], STDIN_FILENO) < 0 ||
        dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(stdout_pipe[1], STDERR_FILENO) < 0) {
        int saved = errno;
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        errno = saved;
        return false;
    }

    pscalRuntimeRegisterVirtualTTYFd(STDIN_FILENO, STDIN_FILENO);
    pscalRuntimeRegisterVirtualTTYFd(STDOUT_FILENO, STDOUT_FILENO);
    pscalRuntimeRegisterVirtualTTYFd(STDERR_FILENO, STDERR_FILENO);
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    *out_master_fd = stdout_pipe[0];
    *out_input_fd = stdin_pipe[1];
    return true;
}

int PSCALRuntimeLaunchExsh(int argc, char* argv[]) {
    NSLog(@"PSCALRuntime: launching exsh (argc=%d)", argc);
    pthread_mutex_lock(&s_runtime_mutex);
    if (s_runtime_active) {
        pthread_mutex_unlock(&s_runtime_mutex);
        NSLog(@"PSCALRuntime: launch aborted (already running)");
        errno = EBUSY;
        return -1;
    }

    bool using_virtual_tty = false;
    int master_fd = -1;
    int slave_fd = -1;
    int input_fd = -1;
    if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) != 0) {
        int err = errno;
        NSLog(@"PSCALRuntime: openpty failed (%d: %s); falling back to pipes", err, strerror(err));
        if (!PSCALRuntimeInstallVirtualTTY(&master_fd, &input_fd)) {
            pthread_mutex_unlock(&s_runtime_mutex);
            errno = err;
            return -1;
        }
        using_virtual_tty = true;
        NSLog(@"PSCALRuntime: using virtual terminal pipes (master=%d)", master_fd);
    } else {
        PSCALRuntimeConfigurePtySlave(slave_fd);
        input_fd = master_fd;
        NSLog(@"PSCALRuntime: openpty succeeded (master=%d)", master_fd);
    }

    s_master_fd = master_fd;
    s_input_fd = input_fd;
    s_using_virtual_tty = using_virtual_tty;
    s_runtime_active = true;
    s_runtime_thread = pthread_self();
    const int initial_columns = s_pending_columns;
    const int initial_rows = s_pending_rows;
    pthread_mutex_unlock(&s_runtime_mutex);

    if (!using_virtual_tty) {
        PSCALRuntimeApplyWindowSize(master_fd, initial_columns, initial_rows);
        pscalRuntimeSetVirtualTTYEnabled(false);
    } else {
        pscalRuntimeSetVirtualTTYEnabled(true);
    }

    // Ensure stdio is line-buffered at most to reduce latency.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (!using_virtual_tty) {
        // Redirect stdio to the PTY slave so exsh sees a real terminal.
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        close(slave_fd);
    }

    pthread_create(&s_output_thread, NULL, PSCALRuntimeOutputPump, NULL);
    NSLog(@"PSCALRuntime: output pump thread started");

    int result = exsh_main(argc, argv);
    NSLog(@"PSCALRuntime: exsh_main exited with status %d", result);

    // Tear down PTY + output pump.
    pthread_mutex_lock(&s_runtime_mutex);
    int pump_fd = s_master_fd;
    int stdin_fd = s_input_fd;
    bool virtual_tty = s_using_virtual_tty;
    s_master_fd = -1;
    s_input_fd = -1;
    s_using_virtual_tty = false;
    s_runtime_active = false;
    memset(&s_runtime_thread, 0, sizeof(s_runtime_thread));
    pthread_mutex_unlock(&s_runtime_mutex);

    if (pump_fd >= 0) {
        close(pump_fd);
    }
    if (stdin_fd >= 0 && stdin_fd != pump_fd) {
        close(stdin_fd);
    }
    pthread_join(s_output_thread, NULL);
    if (virtual_tty) {
        pscalRuntimeSetVirtualTTYEnabled(false);
    }

    pthread_mutex_lock(&s_runtime_mutex);
    PSCALRuntimeExitHandler exit_handler = s_exit_handler;
    void *context = s_handler_context;
    pthread_mutex_unlock(&s_runtime_mutex);

    if (exit_handler) {
        exit_handler(result, context);
    }

    return result;
}

typedef struct {
    int argc;
    char **argv;
    int result;
} PSCALRuntimeThreadContext;

static void *PSCALRuntimeThreadMain(void *arg) {
    PSCALRuntimeThreadContext *context = (PSCALRuntimeThreadContext *)arg;
    context->result = PSCALRuntimeLaunchExsh(context->argc, context->argv);
    return NULL;
}

int PSCALRuntimeLaunchExshWithStackSize(int argc, char* argv[], size_t stackSizeBytes) {
    const size_t defaultStack = 4ull * 1024ull * 1024ull; // 4 MiB
    const size_t minStack = (size_t)PTHREAD_STACK_MIN;
    size_t requestedStack = stackSizeBytes > 0 ? stackSizeBytes : defaultStack;
    if (requestedStack < minStack) {
        requestedStack = minStack;
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int err = pthread_attr_setstacksize(&attr, requestedStack);
    if (err != 0) {
        pthread_attr_destroy(&attr);
        errno = err;
        return -1;
    }

    PSCALRuntimeThreadContext context = {
        .argc = argc,
        .argv = argv,
        .result = 0
    };
    pthread_t thread;
    err = pthread_create(&thread, &attr, PSCALRuntimeThreadMain, &context);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        errno = err;
        return -1;
    }

    pthread_join(thread, NULL);
    return context.result;
}

void PSCALRuntimeSendInput(const char *utf8, size_t length) {
    if (!utf8 || length == 0) {
        return;
    }
    pthread_mutex_lock(&s_runtime_mutex);
    const int fd = s_input_fd;
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

void PSCALRuntimeUpdateWindowSize(int columns, int rows) {
    if (columns <= 0 || rows <= 0) {
        return;
    }

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", columns);
    setenv("COLUMNS", buffer, 1);
    snprintf(buffer, sizeof(buffer), "%d", rows);
    setenv("LINES", buffer, 1);

    pthread_mutex_lock(&s_runtime_mutex);
    s_pending_columns = columns;
    s_pending_rows = rows;
    int fd = s_master_fd;
    bool active = s_runtime_active;
    pthread_t runtime_thread = s_runtime_thread;
    bool virtual_tty = s_using_virtual_tty;
    pthread_mutex_unlock(&s_runtime_mutex);

    if (!virtual_tty) {
        PSCALRuntimeApplyWindowSize(fd, columns, rows);
    }

    if (active && runtime_thread) {
        pthread_kill(runtime_thread, SIGWINCH);
    }
}

int PSCALRuntimeIsVirtualTTY(void) {
    return pscalRuntimeVirtualTTYEnabled() ? 1 : 0;
}
#import <Foundation/Foundation.h>
