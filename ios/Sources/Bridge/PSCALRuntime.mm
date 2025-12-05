#include "PSCALRuntime.h"
#include "PSCALBuildConfig.h"

#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#if __has_feature(modules)
@import Security;
@import CoreServices;
@import SystemConfiguration;
#else
#include <Security/Security.h>
#include <CoreServices/CoreServices.h>
#include <SystemConfiguration/SystemConfiguration.h>
#endif
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

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
extern "C" const char *__asan_default_options(void) {
    // Prevent ASan from manipulating alternate signal stacks and from using
    // a fake stack that can trigger teardown aborts on thread exit.
    return "use_sigaltstack=0 detect_stack_use_after_return=0";
}
#endif
#endif

#if defined(PSCAL_TARGET_IOS) && (PSCAL_BUILD_SDL || PSCAL_BUILD_SDL3)
extern "C" void SDL_SetMainReady(void);
static void PSCALRuntimeEnsureSDLReady(void);
#endif

#define PSCAL_HAS_ASAN_INTERFACE 0

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
static pthread_t s_runtime_thread_id;

static pthread_mutex_t s_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_runtime_active = false;
static int s_master_fd = -1;
static int s_input_fd = -1;
static bool s_using_virtual_tty = false;
static pthread_t s_output_thread;
static pthread_t s_runtime_thread;
static int s_pending_columns = 0;
static int s_pending_rows = 0;
static pthread_mutex_t s_vtty_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_vtty_initialized = false;
static struct termios s_vtty_termios;
static std::string s_vtty_canonical_buffer;

static UIFont *PSCALRuntimeResolveDefaultUIFont(void) {
    CGFloat pointSize = 14.0;
    const char *sizeEnv = getenv("PSCALI_FONT_SIZE");
    if (sizeEnv) {
        double parsed = atof(sizeEnv);
        if (parsed > 0.0) {
            pointSize = (CGFloat)parsed;
        }
    }
    UIFont *resolved = nil;
    const char *fontEnv = getenv("PSCALI_FONT_NAME");
    if (fontEnv && fontEnv[0] != '\0' && fontEnv[0] != '.') {
        NSString *fontName = [NSString stringWithUTF8String:fontEnv];
        if (fontName.length > 0) {
            resolved = [UIFont fontWithName:fontName size:pointSize];
        }
    }
    if (!resolved) {
        resolved = [UIFont monospacedSystemFontOfSize:pointSize weight:UIFontWeightRegular];
    }
    return resolved;
}

static void PSCALRuntimeEnsurePendingWindowSizeLocked(void) {
    if (s_pending_columns > 0 && s_pending_rows > 0) {
        return;
    }
    UIFont *font = PSCALRuntimeResolveDefaultUIFont();
    CGSize screenSize = UIScreen.mainScreen.bounds.size;
    NSDictionary *attributes = @{ NSFontAttributeName: font };
    CGFloat charWidth = MAX(1.0, [@"W" sizeWithAttributes:attributes].width);
    CGFloat lineHeight = MAX(1.0, font.lineHeight);
    int computedColumns = (int)floor(screenSize.width / charWidth);
    int computedRows = (int)floor(screenSize.height / lineHeight);
    s_pending_columns = MAX(10, computedColumns);
    s_pending_rows = MAX(4, computedRows);
}

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

static void PSCALRuntimeInitVirtualTermiosLocked(void) {
    if (s_vtty_initialized) {
        return;
    }
    memset(&s_vtty_termios, 0, sizeof(s_vtty_termios));
    s_vtty_termios.c_iflag = ICRNL | IXON;
#ifdef IUTF8
    s_vtty_termios.c_iflag |= IUTF8;
#endif
    s_vtty_termios.c_oflag = OPOST | ONLCR;
    s_vtty_termios.c_cflag = CS8 | CREAD;
    s_vtty_termios.c_lflag = ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG;
    s_vtty_termios.c_cc[VINTR] = 0x03;   // Ctrl+C
    s_vtty_termios.c_cc[VQUIT] = 0x1c;   // Ctrl+\
    s_vtty_termios.c_cc[VSUSP] = 0x1a;   // Ctrl+Z
    s_vtty_termios.c_cc[VEOF] = 0x04;    // Ctrl+D
    s_vtty_termios.c_cc[VEOL] = '\n';
    s_vtty_termios.c_cc[VEOL2] = '\r';
    s_vtty_initialized = true;
}

static void PSCALRuntimeWriteWithBackoff(int fd, const char *data, size_t length) {
    if (fd < 0 || !data || length == 0) {
        return;
    }
    size_t written = 0;
    int backoffMicros = 1000;
    while (written < length) {
        ssize_t chunk = write(fd, data + written, length - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep((useconds_t)backoffMicros);
                if (backoffMicros < 16000) {
                    backoffMicros *= 2;
                }
                continue;
            }
            break;
        }
        backoffMicros = 1000;
        written += (size_t)chunk;
    }
}

static void PSCALRuntimeEchoToTerminal(const char *data, size_t length) {
    pthread_mutex_lock(&s_runtime_mutex);
    int fd = s_master_fd;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (fd >= 0) {
        PSCALRuntimeWriteWithBackoff(fd, data, length);
    }
}

static void PSCALRuntimeHandleSignalsForByte(uint8_t byte) {
    pthread_mutex_lock(&s_vtty_mutex);
    if (!s_vtty_initialized) {
        PSCALRuntimeInitVirtualTermiosLocked();
    }
    struct termios t = s_vtty_termios;
    pthread_mutex_unlock(&s_vtty_mutex);

    if ((t.c_lflag & ISIG) == 0) {
        return;
    }
    if (byte == t.c_cc[VINTR]) {
        PSCALRuntimeSendSignal(SIGINT);
    } else if (byte == t.c_cc[VQUIT]) {
        PSCALRuntimeSendSignal(SIGQUIT);
    } else if (byte == t.c_cc[VSUSP]) {
        PSCALRuntimeSendSignal(SIGTSTP);
    }
}

static void PSCALRuntimeProcessVirtualTTYInput(const char *utf8, size_t length, int input_fd) {
    pthread_mutex_lock(&s_vtty_mutex);
    if (!s_vtty_initialized) {
        PSCALRuntimeInitVirtualTermiosLocked();
    }
    struct termios t = s_vtty_termios;
    pthread_mutex_unlock(&s_vtty_mutex);

    const bool canonical = (t.c_lflag & ICANON) != 0;
    const bool echo = (t.c_lflag & ECHO) != 0;
    const bool echonl = (t.c_lflag & ECHONL) != 0;

    auto flushCanonical = [&](bool appendNewline) {
        pthread_mutex_lock(&s_vtty_mutex);
        std::string buf = s_vtty_canonical_buffer;
        s_vtty_canonical_buffer.clear();
        pthread_mutex_unlock(&s_vtty_mutex);
        if (buf.empty() && !appendNewline) {
            return;
        }
        if (appendNewline) {
            buf.push_back('\n');
        }
        PSCALRuntimeWriteWithBackoff(input_fd, buf.data(), buf.size());
        if (echo) {
            PSCALRuntimeEchoToTerminal(buf.data(), buf.size());
        }
    };

    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = (uint8_t)utf8[i];
        PSCALRuntimeHandleSignalsForByte(byte);
        if (canonical) {
            if (byte == t.c_cc[VEOF]) {
                flushCanonical(false);
                continue;
            }
            if (byte == '\n' || byte == t.c_cc[VEOL] || byte == t.c_cc[VEOL2]) {
                flushCanonical(true);
                if (echonl && !echo) {
                    char nl = '\n';
                    PSCALRuntimeEchoToTerminal(&nl, 1);
                }
                continue;
            }
            if ((t.c_lflag & ECHOE) && byte == 0x7f) { // DEL erase
                pthread_mutex_lock(&s_vtty_mutex);
                if (!s_vtty_canonical_buffer.empty()) {
                    s_vtty_canonical_buffer.pop_back();
                }
                pthread_mutex_unlock(&s_vtty_mutex);
                continue;
            }
            pthread_mutex_lock(&s_vtty_mutex);
            s_vtty_canonical_buffer.push_back((char)byte);
            pthread_mutex_unlock(&s_vtty_mutex);
            if (echo) {
                PSCALRuntimeEchoToTerminal((const char *)&byte, 1);
            }
        } else {
            // Raw mode: emit immediately
            PSCALRuntimeWriteWithBackoff(input_fd, (const char *)&byte, 1);
            if (echo) {
                PSCALRuntimeEchoToTerminal((const char *)&byte, 1);
            } else if (echonl && (byte == '\n' || byte == '\r')) {
                PSCALRuntimeEchoToTerminal((const char *)&byte, 1);
            }
        }
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

    pthread_mutex_lock(&s_vtty_mutex);
    s_vtty_initialized = false;
    s_vtty_canonical_buffer.clear();
    pthread_mutex_unlock(&s_vtty_mutex);
    return true;
}

int PSCALRuntimeLaunchExsh(int argc, char* argv[]) {
    NSLog(@"PSCALRuntime: launching exsh (argc=%d)", argc);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    // Ensure ASan does not manipulate alt/fake stacks when present.
#if __has_feature(address_sanitizer)
    {
        const char *asan_opts = getenv("ASAN_OPTIONS");
        std::string merged = asan_opts ? std::string(asan_opts) : std::string();
        auto appendOpt = [&merged](const char *opt) {
            if (merged.find(opt) != std::string::npos) {
                return;
            }
            if (!merged.empty() && merged.back() != ' ') {
                merged.push_back(' ');
            }
            merged += opt;
        };
        appendOpt("use_sigaltstack=0");
        appendOpt("detect_stack_use_after_return=0");
        setenv("ASAN_OPTIONS", merged.c_str(), 1);
    }
#endif
    pthread_mutex_lock(&s_runtime_mutex);
    if (s_runtime_active) {
        pthread_mutex_unlock(&s_runtime_mutex);
        NSLog(@"PSCALRuntime: launch aborted (already running)");
        errno = EBUSY;
        return -1;
    }
    PSCALRuntimeEnsurePendingWindowSizeLocked();

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
    s_runtime_thread_id = s_runtime_thread;
    const int initial_columns = s_pending_columns;
    const int initial_rows = s_pending_rows;
    pthread_mutex_unlock(&s_runtime_mutex);

    // Make input writes non-blocking; PSCALRuntimeSendInput will back off on EAGAIN.
    if (input_fd >= 0) {
        int flags = fcntl(input_fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(input_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    if (!using_virtual_tty) {
        PSCALRuntimeApplyWindowSize(master_fd, initial_columns, initial_rows);
        pscalRuntimeSetVirtualTTYEnabled(false);
    } else {
        pscalRuntimeSetVirtualTTYEnabled(true);
    }

    // Ensure session-heavy tools (e.g., embedded Elvis) write into a
    // writable sandbox directory instead of the app bundle.
    @autoreleasepool {
        // Prefer a stable Documents/home working directory so tools like Elvis
        // can create temp/session files in a writable location.
        NSString *homeRoot = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/home"];
        NSError *dirError = nil;
        [[NSFileManager defaultManager] createDirectoryAtPath:homeRoot
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:&dirError];
        NSString *sessionRoot = homeRoot;
        if (sessionRoot.length == 0) {
            sessionRoot = NSTemporaryDirectory();
        }
        if (sessionRoot.length > 0) {
            setenv("HOME", sessionRoot.UTF8String, 1);
            setenv("SESSIONPATH", sessionRoot.UTF8String, 1);
            setenv("TMPDIR", sessionRoot.UTF8String, 1);
            setenv("PWD", sessionRoot.UTF8String, 1);
            chdir(sessionRoot.UTF8String);
        }
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

    pthread_attr_t pumpAttr;
    pthread_attr_init(&pumpAttr);
    const size_t pumpStack = 2ull * 1024ull * 1024ull; // 2 MiB for the pump thread
    pthread_attr_setstacksize(&pumpAttr, pumpStack);
    pthread_create(&s_output_thread, &pumpAttr, PSCALRuntimeOutputPump, NULL);
    pthread_attr_destroy(&pumpAttr);
    NSLog(@"PSCALRuntime: output pump thread started");

#if defined(PSCAL_TARGET_IOS) && (PSCAL_BUILD_SDL || PSCAL_BUILD_SDL3)
    PSCALRuntimeEnsureSDLReady();
#endif
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
    memset(&s_runtime_thread_id, 0, sizeof(s_runtime_thread_id));
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
    const size_t defaultStack = 16ull * 1024ull * 1024ull; // 16 MiB
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
    const bool virtual_tty = s_using_virtual_tty;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (fd < 0) {
        return;
    }
    if (virtual_tty) {
        PSCALRuntimeProcessVirtualTTYInput(utf8, length, fd);
    } else {
        PSCALRuntimeWriteWithBackoff(fd, utf8, length);
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

void PSCALRuntimeSendSignal(int signo) {
    pthread_mutex_lock(&s_runtime_mutex);
    bool active = s_runtime_active;
    pthread_t target = s_runtime_thread_id;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (!active || target == 0) {
        return;
    }
    pthread_kill(target, signo);
}

void PSCALRuntimeApplyPathTruncation(const char *path) {
    if (path && path[0] != '\0') {
        setenv("PATH_TRUNCATE", path, 1);
    } else {
        unsetenv("PATH_TRUNCATE");
    }
}

extern "C" int pscalPlatformClipboardSet(const char *utf8, size_t len) {
#if defined(PSCAL_TARGET_IOS)
    if (!utf8) return -1;
    NSString *str = [[NSString alloc] initWithBytes:utf8 length:len encoding:NSUTF8StringEncoding];
    if (!str) return -1;
    UIPasteboard.generalPasteboard.string = str;
    return 0;
#else
    (void)utf8; (void)len;
    return -1;
#endif
}

extern "C" char *pscalPlatformClipboardGet(size_t *out_len) {
#if defined(PSCAL_TARGET_IOS)
    NSString *str = UIPasteboard.generalPasteboard.string;
    if (!str) return NULL;
    NSData *data = [str dataUsingEncoding:NSUTF8StringEncoding];
    if (!data) return NULL;
    size_t len = (size_t)data.length;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, data.bytes, len);
    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
#else
    (void)out_len;
    return NULL;
#endif
}
#import <Foundation/Foundation.h>
#if defined(PSCAL_TARGET_IOS) && (PSCAL_BUILD_SDL || PSCAL_BUILD_SDL3)
static void PSCALRuntimeEnsureSDLReady(void) {
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        SDL_SetMainReady();
    });
}
#endif
