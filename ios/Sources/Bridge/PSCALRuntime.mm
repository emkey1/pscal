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
#include <TargetConditionals.h>
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
#include <setjmp.h>
#include <atomic>
#include <string>
#include <termios.h>
#if __has_include(<util.h>)
#include <util.h>
#else
#include <pty.h>
#endif
#include "common/runtime_tty.h"
#include "ios/vproc.h"
#include "ios/tty/pscal_fd.h"
#include "ios/tty/pscal_pty.h"
extern "C" {
#include "backend_ast/builtin.h"
}

#if TARGET_OS_IPHONE
extern "C" int PSCALRuntimeIsRunning(void);
static thread_local jmp_buf *s_exit_jump_buffer = nullptr;
extern "C" void exit(int status) {
    if (s_exit_jump_buffer) {
        // Convert exit to a longjmp so the runtime thread can unwind cleanly.
        longjmp(*s_exit_jump_buffer, status == 0 ? 1 : status);
    }
    if (PSCALRuntimeIsRunning()) {
        NSLog(@"PSCALRuntime: intercepted exit(%d); converting to pthread_exit on iOS", status);
        pthread_exit((void *)(intptr_t)status);
    }
    _Exit(status);
}
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
    char *pscalRuntimeCopyMarketingVersion(void);
    void shellRuntimeInitSignals(void);
    ShellRuntimeState *shellRuntimeCreateContext(void);
    ShellRuntimeState *shellRuntimeActivateContext(ShellRuntimeState *ctx);
    void shellRuntimeDestroyContext(ShellRuntimeState *ctx);
}

static PSCALRuntimeOutputHandler s_output_handler = NULL;
static PSCALRuntimeExitHandler s_exit_handler = NULL;
static void *s_handler_context = NULL;
static std::atomic<size_t> s_output_backlog(0);
static const size_t kOutputBacklogLimit = 512 * 1024;
static const useconds_t kOutputBackpressureSleepUsec = 1000;
static const size_t kOutputRingCapacity = 512 * 1024;
static std::atomic<int> s_output_buffering_enabled{0};
static pthread_once_t s_output_ring_once = PTHREAD_ONCE_INIT;

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t size;
    bool active;
    pthread_mutex_t lock;
    pthread_cond_t can_read;
    pthread_cond_t can_write;
} PSCALRuntimeOutputRing;

static PSCALRuntimeOutputRing s_output_ring = {};
static pthread_t s_runtime_thread_id;
static ShellRuntimeState *s_forced_shell_ctx = NULL;
static bool s_forced_shell_ctx_owned = false;
static pthread_mutex_t s_session_exit_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_session_exit_cv = PTHREAD_COND_INITIALIZER;
static bool s_session_exit_pending = false;
static int s_session_exit_status = 0;

static BOOL PSCALRuntimeFontNameAllowed(NSString *candidate) {
    if (!candidate) {
        return NO;
    }
    NSString *trimmed = [candidate stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
    if (trimmed.length == 0) {
        return NO;
    }
    if ([trimmed hasPrefix:@"."]) {
        return NO;
    }
    NSString *lower = trimmed.lowercaseString;
    if ([lower hasPrefix:@"sfcompact"]) {
        return NO;
    }
    return YES;
}

static pthread_mutex_t s_runtime_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool s_runtime_active = false;
static int s_master_fd = -1;
static int s_input_fd = -1;
static bool s_using_virtual_tty = false;
static bool s_using_pscal_pty = false;
static uint64_t s_runtime_session_id = 0;
static VProcSessionStdio *s_runtime_stdio = NULL;
static pthread_t s_output_thread;
static pthread_t s_runtime_thread;
static int s_pending_columns = 0;
static int s_pending_rows = 0;
static pthread_mutex_t s_vtty_mutex = PTHREAD_MUTEX_INITIALIZER;
static std::string s_vtty_canonical_buffer;
static std::string s_vtty_raw_buffer;
static int s_runtime_log_fd = -1;
static int s_script_capture_fd = -1;
static std::string s_script_capture_path;
static std::atomic<int> s_output_log_enabled{-1};

static bool PSCALRuntimeDirectPtyEnabled(void) {
    const char *env = getenv("PSCALI_PTY_OUTPUT_DIRECT");
    if (!env || env[0] == '\0') {
        return true;
    }
    return strcmp(env, "0") != 0;
}

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
    if (fontEnv && fontEnv[0] != '\0') {
        NSString *fontName = [NSString stringWithUTF8String:fontEnv];
        if (PSCALRuntimeFontNameAllowed(fontName)) {
            NSString *trimmed = [fontName stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
            resolved = [UIFont fontWithName:trimmed size:pointSize];
            if (resolved) {
                UIFontDescriptorSymbolicTraits traits = resolved.fontDescriptor.symbolicTraits;
                if ((traits & UIFontDescriptorTraitMonoSpace) == 0) {
                    resolved = nil;
                }
            }
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

static void PSCALRuntimeCloseFd(int *fd) {
    if (fd && *fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static __attribute__((unused)) bool PSCALRuntimeInstallPscalPty(int *out_master_fd,
                                                                int *out_input_fd,
                                                                VProcSessionStdio **out_stdio,
                                                                uint64_t *out_session_id) {
    if (!out_master_fd || !out_input_fd || !out_stdio || !out_session_id) {
        errno = EINVAL;
        return false;
    }
    int session_stdin = -1;
    int session_stdout = -1;
    int ui_read = -1;
    int ui_write = -1;
    if (vprocCreateSessionPipes(&session_stdin, &session_stdout, &ui_read, &ui_write) != 0) {
        return false;
    }
    struct pscal_fd *pty_master = NULL;
    struct pscal_fd *pty_slave = NULL;
    int pty_num = -1;
    int pty_err = pscalPtyOpenMaster(O_RDWR, &pty_master, &pty_num);
    if (pty_err < 0) {
        PSCALRuntimeCloseFd(&session_stdin);
        PSCALRuntimeCloseFd(&session_stdout);
        PSCALRuntimeCloseFd(&ui_read);
        PSCALRuntimeCloseFd(&ui_write);
        errno = pscalCompatErrno(pty_err);
        return false;
    }
    pty_err = pscalPtyUnlock(pty_master);
    if (pty_err < 0) {
        PSCALRuntimeCloseFd(&session_stdin);
        PSCALRuntimeCloseFd(&session_stdout);
        PSCALRuntimeCloseFd(&ui_read);
        PSCALRuntimeCloseFd(&ui_write);
        if (pty_master) {
            pscal_fd_close(pty_master);
        }
        errno = pscalCompatErrno(pty_err);
        return false;
    }
    pty_err = pscalPtyOpenSlave(pty_num, O_RDWR, &pty_slave);
    if (pty_err < 0) {
        PSCALRuntimeCloseFd(&session_stdin);
        PSCALRuntimeCloseFd(&session_stdout);
        PSCALRuntimeCloseFd(&ui_read);
        PSCALRuntimeCloseFd(&ui_write);
        if (pty_master) {
            pscal_fd_close(pty_master);
        }
        errno = pscalCompatErrno(pty_err);
        return false;
    }

    VProcSessionStdio *stdio_ctx = vprocSessionStdioCreate();
    if (!stdio_ctx) {
        PSCALRuntimeCloseFd(&session_stdin);
        PSCALRuntimeCloseFd(&session_stdout);
        PSCALRuntimeCloseFd(&ui_read);
        PSCALRuntimeCloseFd(&ui_write);
        if (pty_master) {
            pscal_fd_close(pty_master);
        }
        if (pty_slave) {
            pscal_fd_close(pty_slave);
        }
        errno = ENOMEM;
        return false;
    }

    int kernel_pid = vprocEnsureKernelPid();
    const uint64_t session_id = 1;
    if (vprocSessionStdioInitWithPty(stdio_ctx,
                                     pty_slave,
                                     pty_master,
                                     session_stdin,
                                     session_stdout,
                                     session_id,
                                     kernel_pid) != 0) {
        int saved = errno;
        vprocSessionStdioDestroy(stdio_ctx);
        PSCALRuntimeCloseFd(&ui_read);
        PSCALRuntimeCloseFd(&ui_write);
        errno = saved;
        return false;
    }

    if (dup2(session_stdout, STDOUT_FILENO) < 0 || dup2(session_stdout, STDERR_FILENO) < 0) {
        int saved = errno;
        vprocSessionStdioDestroy(stdio_ctx);
        PSCALRuntimeCloseFd(&ui_read);
        PSCALRuntimeCloseFd(&ui_write);
        errno = saved;
        return false;
    }

    vprocSessionStdioActivate(stdio_ctx);
    *out_master_fd = ui_read;
    *out_input_fd = ui_write;
    *out_stdio = stdio_ctx;
    *out_session_id = session_id;
    return true;
}

static __attribute__((unused)) void PSCALRuntimeConfigurePtySlave(int fd) {
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

void PSCALRuntimeRegisterSessionOutputHandler(uint64_t session_id,
                                              PSCALRuntimeSessionOutputHandler handler,
                                              void *context) {
    if (session_id == 0) {
        return;
    }
    vprocSessionSetOutputHandler(session_id, (VProcSessionOutputHandler)handler, context);
}

void PSCALRuntimeUnregisterSessionOutputHandler(uint64_t session_id) {
    if (session_id == 0) {
        return;
    }
    vprocSessionClearOutputHandler(session_id);
}

extern "C" ShellRuntimeState *pscalRuntimeShellContextForSession(uint64_t session_id) {
    if (session_id != 1) {
        return NULL;
    }
    pthread_mutex_lock(&s_runtime_mutex);
    ShellRuntimeState *ctx = s_forced_shell_ctx;
    pthread_mutex_unlock(&s_runtime_mutex);
    return ctx;
}

extern "C" void pscalRuntimeRegisterShellThread(uint64_t session_id, pthread_t tid) {
    pthread_mutex_lock(&s_runtime_mutex);
    if (s_runtime_active && session_id == s_runtime_session_id) {
        s_runtime_thread_id = tid;
    }
    pthread_mutex_unlock(&s_runtime_mutex);
}

extern "C" void pscalRuntimeKernelSessionExited(uint64_t session_id, int status) {
    bool match = false;
    pthread_mutex_lock(&s_runtime_mutex);
    match = s_runtime_active && session_id == s_runtime_session_id;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (!match) {
        return;
    }
    pthread_mutex_lock(&s_session_exit_mu);
    s_session_exit_status = status;
    s_session_exit_pending = true;
    pthread_cond_broadcast(&s_session_exit_cv);
    pthread_mutex_unlock(&s_session_exit_mu);
}

static void PSCALRuntimeDispatchOutput(const char *buffer, size_t length) {
    pthread_mutex_lock(&s_runtime_mutex);
    PSCALRuntimeOutputHandler handler = s_output_handler;
    void *context = s_handler_context;
    pthread_mutex_unlock(&s_runtime_mutex);

    if (handler && buffer && length > 0) {
        s_output_backlog.fetch_add(length, std::memory_order_relaxed);
        char *heap_buffer = (char *)malloc(length);
        if (!heap_buffer) {
            s_output_backlog.fetch_sub(length, std::memory_order_relaxed);
            return;
        }
        memcpy(heap_buffer, buffer, length);
        handler(heap_buffer, length, context);
    }
}

extern "C" void PSCALRuntimeOutputDidProcess(size_t length) {
    if (length == 0) {
        return;
    }
    size_t prev = s_output_backlog.fetch_sub(length, std::memory_order_relaxed);
    if (length > prev) {
        s_output_backlog.store(0, std::memory_order_relaxed);
    }
}
/***********/
static bool PSCALRuntimeShouldSuppressLogLine(const std::string &line) {
    std::string trimmed = line;
    while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r')) {
        trimmed.pop_back();
    }
    // Suppress noisy GPS NMEA sentences that may surface if a reader is attached to /dev/location.
    if (!trimmed.empty() && trimmed.rfind("$GP", 0) == 0) {
        return true;
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

static void PSCALRuntimeOutputRingInitOnce(void) {
    pthread_mutex_init(&s_output_ring.lock, NULL);
    pthread_cond_init(&s_output_ring.can_read, NULL);
    pthread_cond_init(&s_output_ring.can_write, NULL);
}

static void PSCALRuntimeOutputRingActivate(void) {
    pthread_once(&s_output_ring_once, PSCALRuntimeOutputRingInitOnce);
    pthread_mutex_lock(&s_output_ring.lock);
    if (!s_output_ring.data) {
        s_output_ring.capacity = kOutputRingCapacity;
        s_output_ring.data = (uint8_t *)malloc(s_output_ring.capacity);
        s_output_ring.head = 0;
        s_output_ring.tail = 0;
        s_output_ring.size = 0;
    } else {
        s_output_ring.head = 0;
        s_output_ring.tail = 0;
        s_output_ring.size = 0;
    }
    s_output_ring.active = (s_output_ring.data != NULL);
    pthread_cond_broadcast(&s_output_ring.can_write);
    pthread_cond_broadcast(&s_output_ring.can_read);
    pthread_mutex_unlock(&s_output_ring.lock);
}

static void PSCALRuntimeOutputRingDeactivate(void) {
    pthread_once(&s_output_ring_once, PSCALRuntimeOutputRingInitOnce);
    pthread_mutex_lock(&s_output_ring.lock);
    s_output_ring.active = false;
    s_output_ring.head = 0;
    s_output_ring.tail = 0;
    s_output_ring.size = 0;
    pthread_cond_broadcast(&s_output_ring.can_write);
    pthread_cond_broadcast(&s_output_ring.can_read);
    pthread_mutex_unlock(&s_output_ring.lock);
}

static bool PSCALRuntimeOutputBufferingEnabled(void) {
    return s_output_buffering_enabled.load(std::memory_order_acquire) != 0;
}

void PSCALRuntimeSetOutputBufferingEnabled(int enabled) {
    if (enabled) {
        PSCALRuntimeOutputRingActivate();
        s_output_buffering_enabled.store(1, std::memory_order_release);
    } else {
        s_output_buffering_enabled.store(0, std::memory_order_release);
        PSCALRuntimeOutputRingDeactivate();
    }
}

static bool PSCALRuntimeQueueOutput(const char *buffer, size_t length) {
    if (!buffer || length == 0) {
        return true;
    }
    if (!PSCALRuntimeOutputBufferingEnabled()) {
        return false;
    }
    pthread_once(&s_output_ring_once, PSCALRuntimeOutputRingInitOnce);
    pthread_mutex_lock(&s_output_ring.lock);
    if (!s_output_ring.data || s_output_ring.capacity == 0) {
        pthread_mutex_unlock(&s_output_ring.lock);
        return false;
    }
    if (!s_output_ring.active) {
        pthread_mutex_unlock(&s_output_ring.lock);
        return true;
    }
    size_t offset = 0;
    while (offset < length && s_output_ring.active) {
        while (s_output_ring.size == s_output_ring.capacity && s_output_ring.active) {
            pthread_cond_wait(&s_output_ring.can_write, &s_output_ring.lock);
        }
        if (!s_output_ring.active) {
            break;
        }
        size_t space = s_output_ring.capacity - s_output_ring.size;
        size_t remaining = length - offset;
        size_t chunk = remaining < space ? remaining : space;
        size_t tail = s_output_ring.tail;
        size_t first = s_output_ring.capacity - tail;
        if (first > chunk) {
            first = chunk;
        }
        memcpy(s_output_ring.data + tail, buffer + offset, first);
        tail = (tail + first) % s_output_ring.capacity;
        s_output_ring.size += first;
        offset += first;
        size_t second = chunk - first;
        if (second > 0) {
            memcpy(s_output_ring.data + tail, buffer + offset, second);
            tail = (tail + second) % s_output_ring.capacity;
            s_output_ring.size += second;
            offset += second;
        }
        s_output_ring.tail = tail;
        pthread_cond_signal(&s_output_ring.can_read);
    }
    pthread_mutex_unlock(&s_output_ring.lock);
    return offset == length;
}

size_t PSCALRuntimeDrainOutput(uint8_t **out_buffer, size_t max_bytes) {
    if (!out_buffer || max_bytes == 0) {
        return 0;
    }
    *out_buffer = NULL;
    if (!PSCALRuntimeOutputBufferingEnabled()) {
        return 0;
    }
    pthread_once(&s_output_ring_once, PSCALRuntimeOutputRingInitOnce);
    pthread_mutex_lock(&s_output_ring.lock);
    if (!s_output_ring.active || s_output_ring.size == 0 || !s_output_ring.data) {
        pthread_mutex_unlock(&s_output_ring.lock);
        return 0;
    }
    size_t to_read = s_output_ring.size < max_bytes ? s_output_ring.size : max_bytes;
    uint8_t *buffer = (uint8_t *)malloc(to_read);
    if (!buffer) {
        pthread_mutex_unlock(&s_output_ring.lock);
        return 0;
    }
    size_t head = s_output_ring.head;
    size_t first = s_output_ring.capacity - head;
    if (first > to_read) {
        first = to_read;
    }
    memcpy(buffer, s_output_ring.data + head, first);
    s_output_ring.head = (head + first) % s_output_ring.capacity;
    s_output_ring.size -= first;
    size_t remaining = to_read - first;
    if (remaining > 0) {
        memcpy(buffer + first, s_output_ring.data + s_output_ring.head, remaining);
        s_output_ring.head = (s_output_ring.head + remaining) % s_output_ring.capacity;
        s_output_ring.size -= remaining;
    }
    pthread_cond_signal(&s_output_ring.can_write);
    pthread_mutex_unlock(&s_output_ring.lock);
    *out_buffer = buffer;
    return to_read;
}

static bool PSCALRuntimeOutputLoggingEnabled(void) {
    int cached = s_output_log_enabled.load(std::memory_order_acquire);
    if (cached >= 0) {
        return cached != 0;
    }
    const char *env = getenv("PSCALI_PTY_OUTPUT_LOG");
    int enabled = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
    s_output_log_enabled.store(enabled, std::memory_order_release);
    return enabled != 0;
}

static bool PSCALRuntimeEnsureLogFd(void) {
    if (!PSCALRuntimeOutputLoggingEnabled()) {
        return false;
    }
    if (s_runtime_log_fd >= 0) {
        return true;
    }
    @autoreleasepool {
        NSString *root = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/var/log"];
        if (root.length == 0) {
            return false;
        }
        [[NSFileManager defaultManager] createDirectoryAtPath:root
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
        NSString *path = [root stringByAppendingPathComponent:@"pscal_runtime.log"];
        if (path.length == 0) {
            return false;
        }
        int fd = open(path.UTF8String,
                      O_WRONLY | O_CREAT | O_APPEND
#ifdef O_CLOEXEC
                      | O_CLOEXEC
#endif
                      ,
                      0644);
        if (fd >= 0) {
            s_runtime_log_fd = fd;
            return true;
        }
    }
    return false;
}

static void PSCALRuntimeLogOutput(const char *buffer, size_t length) {
    if (!buffer || length == 0) {
        return;
    }
    if (!PSCALRuntimeEnsureLogFd()) {
        return;
    }
    (void)vprocHostWrite(s_runtime_log_fd, buffer, length);
}

static void PSCALRuntimeDirectOutputHandler(uint64_t session_id,
                                            const char *buffer,
                                            size_t length,
                                            void *context) {
    (void)session_id;
    (void)context;
    if (!buffer || length == 0) {
        return;
    }
    PSCALRuntimeLogOutput((const char *)buffer, length);
    pthread_mutex_lock(&s_runtime_mutex);
    int capture_fd = s_script_capture_fd;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (capture_fd >= 0) {
        (void)vprocHostWrite(capture_fd, buffer, length);
    }
    std::string chunk((const char *)buffer, length);
    if (PSCALRuntimeShouldSuppressLogLine(chunk)) {
        return;
    }
    if (PSCALRuntimeQueueOutput((const char *)buffer, length)) {
        return;
    }
    PSCALRuntimeDispatchOutput((const char *)buffer, length);
}

extern "C" __attribute__((used)) void PSCALRuntimeLogLine(const char *message) {
    if (!message || !*message) {
        return;
    }
    PSCALRuntimeLogOutput(message, strlen(message));
    PSCALRuntimeLogOutput("\n", 1);
}

extern "C" void PSCALRuntimeBeginScriptCapture(const char *path, int append) {
    pthread_mutex_lock(&s_runtime_mutex);
    if (s_script_capture_fd >= 0) {
        close(s_script_capture_fd);
        s_script_capture_fd = -1;
        s_script_capture_path.clear();
    }
    pthread_mutex_unlock(&s_runtime_mutex);

    if (!path || path[0] == '\0') {
        return;
    }
    std::string resolved(path);
    @autoreleasepool {
        NSString *nsPath = [NSString stringWithUTF8String:resolved.c_str()];
        if (!nsPath) {
            return;
        }
        NSString *dir = [nsPath stringByDeletingLastPathComponent];
        if (dir.length == 0) {
            dir = @".";
        }
        [[NSFileManager defaultManager] createDirectoryAtPath:dir
                                  withIntermediateDirectories:YES
                                                   attributes:nil
                                                        error:nil];
    }
    int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(resolved.c_str(), flags, 0644);
    if (fd < 0) {
        return;
    }
    pthread_mutex_lock(&s_runtime_mutex);
    s_script_capture_fd = fd;
    s_script_capture_path = resolved;
    pthread_mutex_unlock(&s_runtime_mutex);
}

extern "C" void PSCALRuntimeEndScriptCapture(void) {
    pthread_mutex_lock(&s_runtime_mutex);
    if (s_script_capture_fd >= 0) {
        close(s_script_capture_fd);
        s_script_capture_fd = -1;
    }
    s_script_capture_path.clear();
    pthread_mutex_unlock(&s_runtime_mutex);
}

extern "C" int PSCALRuntimeScriptCaptureActive(void) {
    pthread_mutex_lock(&s_runtime_mutex);
    int active = (s_script_capture_fd >= 0);
    pthread_mutex_unlock(&s_runtime_mutex);
    return active;
}

static void PSCALRuntimeWriteWithBackoff(int fd, const char *data, size_t length) {
    if (fd < 0 || !data || length == 0) {
        return;
    }
    size_t written = 0;
    int backoffMicros = 1000;
    while (written < length) {
        ssize_t chunk = vprocHostWrite(fd, data + written, length - written);
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

static void PSCALRuntimeWriteMasterWithBackoff(uint64_t session_id, const char *data, size_t length) {
    if (session_id == 0 || !data || length == 0) {
        return;
    }
    int backoffMicros = 1000;
    while (true) {
        ssize_t wrote = vprocSessionWriteToMaster(session_id, data, length);
        if (wrote < 0) {
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
        }
        break;
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
    struct termios t;
    pscalRuntimeVirtualTTYGetTermios(&t);
    pthread_mutex_unlock(&s_vtty_mutex);

    if ((t.c_lflag & ISIG) == 0) {
        return;
    }
    if (byte == t.c_cc[VINTR]) {
        PSCALRuntimeSendSignal(SIGINT);
    } else if (byte == t.c_cc[VQUIT]) {
        PSCALRuntimeSendSignal(SIGQUIT);
    } else if (byte == t.c_cc[VSUSP]) {
#if defined(PSCAL_TARGET_IOS)
        /* Never propagate SIGTSTP to the host process; ignore Ctrl-Z. */
        return;
#else
        PSCALRuntimeSendSignal(SIGTSTP);
#endif
    }
}

static void PSCALRuntimeProcessVirtualTTYInput(const char *utf8, size_t length, int input_fd) {
    pthread_mutex_lock(&s_vtty_mutex);
    struct termios t;
    pscalRuntimeVirtualTTYGetTermios(&t);
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
            // Raw mode: deliver immediately; avoid double-echo by keeping ECHO off by default.
            PSCALRuntimeWriteWithBackoff(input_fd, (const char *)&byte, 1);
            if (echo || (echonl && (byte == '\n' || byte == '\r'))) {
                PSCALRuntimeEchoToTerminal((const char *)&byte, 1);
            }
        }
    }

    // No deferred raw buffering (we deliver immediately in raw mode).
}
static void *PSCALRuntimeOutputPump(void *_) {
    (void)_;
    vprocRegisterInterposeBypassThread(pthread_self());
    const int fd = s_master_fd;
    char buffer[4096];
    if (PSCALRuntimeOutputBufferingEnabled()) {
        while (true) {
            ssize_t nread = vprocHostRead(fd, buffer, sizeof(buffer));
            if (nread < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            if (nread == 0) {
                break; // EOF
            }
            PSCALRuntimeLogOutput(buffer, (size_t)nread);
            pthread_mutex_lock(&s_runtime_mutex);
            int capture_fd = s_script_capture_fd;
            pthread_mutex_unlock(&s_runtime_mutex);
            if (capture_fd >= 0) {
                (void)vprocHostWrite(capture_fd, buffer, (size_t)nread);
            }
            std::string chunk(buffer, (size_t)nread);
            if (PSCALRuntimeShouldSuppressLogLine(chunk)) {
                continue;
            }
            if (!PSCALRuntimeQueueOutput(buffer, (size_t)nread)) {
                PSCALRuntimeDispatchOutput(buffer, (size_t)nread);
            }
        }
        vprocUnregisterInterposeBypassThread(pthread_self());
        return NULL;
    }
    while (true) {
        size_t backlog = s_output_backlog.load(std::memory_order_relaxed);
        if (backlog >= kOutputBacklogLimit) {
            usleep(kOutputBackpressureSleepUsec);
            continue;
        }
        size_t room = kOutputBacklogLimit - backlog;
        size_t to_read = room < sizeof(buffer) ? room : sizeof(buffer);
        if (to_read == 0) {
            usleep(kOutputBackpressureSleepUsec);
            continue;
        }
        ssize_t nread = vprocHostRead(fd, buffer, to_read);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (nread == 0) {
            break; // EOF
        }
        PSCALRuntimeLogOutput(buffer, (size_t)nread);
        pthread_mutex_lock(&s_runtime_mutex);
        int capture_fd = s_script_capture_fd;
        pthread_mutex_unlock(&s_runtime_mutex);
        if (capture_fd >= 0) {
            (void)vprocHostWrite(capture_fd, buffer, (size_t)nread);
        }
        std::string chunk(buffer, (size_t)nread);
        if (PSCALRuntimeShouldSuppressLogLine(chunk)) {
            continue;
        }
        PSCALRuntimeDispatchOutput(buffer, (size_t)nread);
    }
    vprocUnregisterInterposeBypassThread(pthread_self());
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

static __attribute__((unused)) bool PSCALRuntimeInstallVirtualTTY(int *out_master_fd, int *out_input_fd) {
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
    s_vtty_canonical_buffer.clear();
    pthread_mutex_unlock(&s_vtty_mutex);
    pscalRuntimeVirtualTTYReset();
    return true;
}

int PSCALRuntimeLaunchExsh(int argc, char* argv[]) {
    NSLog(@"PSCALRuntime: launching exsh (argc=%d)", argc);
    PSCALRuntimeInterposeBootstrap();
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

    const int initial_columns = s_pending_columns;
    const int initial_rows = s_pending_rows;
    pthread_mutex_lock(&s_session_exit_mu);
    s_session_exit_pending = false;
    s_session_exit_status = 0;
    pthread_mutex_unlock(&s_session_exit_mu);
    pthread_mutex_unlock(&s_runtime_mutex);

    (void)vprocEnsureKernelPid();
    const uint64_t session_id = 1;
    pthread_mutex_lock(&s_runtime_mutex);
    s_runtime_active = true;
    s_runtime_session_id = session_id;
    s_runtime_thread = pthread_self();
    s_runtime_thread_id = 0;
    pthread_mutex_unlock(&s_runtime_mutex);
    const bool direct_pty = PSCALRuntimeDirectPtyEnabled();
    if (direct_pty) {
        PSCALRuntimeRegisterSessionOutputHandler(session_id, PSCALRuntimeDirectOutputHandler, NULL);
    }
    int master_fd = -1;
    int input_fd = -1;
    if (PSCALRuntimeCreateShellSession(argc, argv, session_id, &master_fd, &input_fd) != 0) {
        pthread_mutex_lock(&s_runtime_mutex);
        s_runtime_active = false;
        s_runtime_session_id = 0;
        memset(&s_runtime_thread, 0, sizeof(s_runtime_thread));
        memset(&s_runtime_thread_id, 0, sizeof(s_runtime_thread_id));
        pthread_mutex_unlock(&s_runtime_mutex);
        if (direct_pty) {
            PSCALRuntimeUnregisterSessionOutputHandler(session_id);
        }
        return -1;
    }

    pthread_mutex_lock(&s_runtime_mutex);
    s_master_fd = direct_pty ? -1 : master_fd;
    s_input_fd = direct_pty ? -1 : input_fd;
    s_using_virtual_tty = false;
    s_using_pscal_pty = true;
    s_runtime_stdio = NULL;
    pthread_mutex_unlock(&s_runtime_mutex);
    s_output_backlog.store(0, std::memory_order_relaxed);
    if (PSCALRuntimeOutputBufferingEnabled()) {
        PSCALRuntimeOutputRingActivate();
    }

    if (direct_pty) {
        if (master_fd >= 0) {
            close(master_fd);
        }
        if (input_fd >= 0) {
            close(input_fd);
        }
    }

    // Make input writes non-blocking; PSCALRuntimeSendInput will back off on EAGAIN.
    if (!direct_pty && input_fd >= 0) {
        int flags = fcntl(input_fd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(input_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    PSCALRuntimeSetSessionWinsize(session_id, initial_columns, initial_rows);
    pscalRuntimeSetVirtualTTYEnabled(false);

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

    if (!direct_pty) {
        pthread_attr_t pumpAttr;
        pthread_attr_init(&pumpAttr);
        const size_t pumpStack = 2ull * 1024ull * 1024ull; // 2 MiB for the pump thread
        pthread_attr_setstacksize(&pumpAttr, pumpStack);
        pthread_create(&s_output_thread, &pumpAttr, PSCALRuntimeOutputPump, NULL);
        pthread_attr_destroy(&pumpAttr);
        NSLog(@"PSCALRuntime: output pump thread started");
    } else {
        memset(&s_output_thread, 0, sizeof(s_output_thread));
    }

#if defined(PSCAL_TARGET_IOS) && (PSCAL_BUILD_SDL || PSCAL_BUILD_SDL3)
    PSCALRuntimeEnsureSDLReady();
#endif

    pthread_mutex_lock(&s_session_exit_mu);
    while (!s_session_exit_pending) {
        pthread_cond_wait(&s_session_exit_cv, &s_session_exit_mu);
    }
    int result = s_session_exit_status;
    s_session_exit_pending = false;
    pthread_mutex_unlock(&s_session_exit_mu);
    NSLog(@"PSCALRuntime: kernel session exited with status %d", result);

    // Tear down PTY + output pump.
    pthread_mutex_lock(&s_runtime_mutex);
    int pump_fd = s_master_fd;
    int stdin_fd = s_input_fd;
    bool virtual_tty = s_using_virtual_tty;
    s_master_fd = -1;
    s_input_fd = -1;
    s_using_virtual_tty = false;
    s_using_pscal_pty = false;
    s_runtime_session_id = 0;
    s_runtime_stdio = NULL;
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
    if (!direct_pty) {
        pthread_join(s_output_thread, NULL);
    } else {
        PSCALRuntimeUnregisterSessionOutputHandler(session_id);
    }
    if (PSCALRuntimeOutputBufferingEnabled()) {
        PSCALRuntimeOutputRingDeactivate();
    }
    PSCALRuntimeEndScriptCapture();
    if (s_runtime_log_fd >= 0) {
        close(s_runtime_log_fd);
        s_runtime_log_fd = -1;
    }
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
    jmp_buf exit_env;
#if TARGET_OS_IPHONE
    s_exit_jump_buffer = &exit_env;
#endif
    int jump_code = setjmp(exit_env);
    if (jump_code == 0) {
        context->result = PSCALRuntimeLaunchExsh(context->argc, context->argv);
    } else {
        context->result = jump_code;
    }
#if TARGET_OS_IPHONE
    s_exit_jump_buffer = nullptr;
#endif
    return NULL;
}

int PSCALRuntimeLaunchExshWithStackSize(int argc, char* argv[], size_t stackSizeBytes) {
    PSCALRuntimeInterposeBootstrap();
    (void)vprocEnsureKernelPid();
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
    const uint64_t session_id = s_runtime_session_id;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (virtual_tty) {
        PSCALRuntimeProcessVirtualTTYInput(utf8, length, fd);
        return;
    }
    if (PSCALRuntimeDirectPtyEnabled() && session_id != 0) {
        PSCALRuntimeWriteMasterWithBackoff(session_id, utf8, length);
        return;
    }
    if (fd < 0) {
        return;
    }
    PSCALRuntimeWriteWithBackoff(fd, utf8, length);
}

void PSCALRuntimeSendInputForSession(uint64_t session_id, const char *utf8, size_t length) {
    if (!utf8 || length == 0 || session_id == 0) {
        return;
    }
    PSCALRuntimeWriteMasterWithBackoff(session_id, utf8, length);
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
    pthread_t runtime_thread = s_runtime_thread_id;
    bool virtual_tty = s_using_virtual_tty;
    bool using_pscal_pty = s_using_pscal_pty;
    uint64_t session_id = s_runtime_session_id;
    pthread_mutex_unlock(&s_runtime_mutex);

    if (using_pscal_pty && session_id != 0) {
        PSCALRuntimeSetSessionWinsize(session_id, columns, rows);
    } else if (!virtual_tty) {
        PSCALRuntimeApplyWindowSize(fd, columns, rows);
    }

    if (active && runtime_thread) {
        pthread_kill(runtime_thread, SIGWINCH);
    }
}

int PSCALRuntimeIsVirtualTTY(void) {
    return pscalRuntimeVirtualTTYEnabled() ? 1 : 0;
}

char *pscalRuntimeCopyMarketingVersion(void) {
    @autoreleasepool {
        NSString *shortVer = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
        NSString *buildVer = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"];
        NSString *version = (shortVer.length > 0) ? shortVer : buildVer;
        if (!version || version.length == 0) {
            return NULL;
        }
        const char *utf8 = [version UTF8String];
        if (!utf8) {
            return NULL;
        }
        return strdup(utf8);
    }
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
    vprocApplyPathTruncation(path);
}

void PSCALRuntimeSetLocationDeviceEnabled(int enabled) {
    vprocLocationDeviceSetEnabled(enabled != 0);
}

int PSCALRuntimeWriteLocationDevice(const char *utf8, size_t length) {
    if (!utf8 || length == 0) {
        return 0;
    }
    ssize_t res = vprocLocationDeviceWrite(utf8, length);
    return res < 0 ? -1 : (int)res;
}

void *PSCALRuntimeCreateShellContext(void) {
    return (void *)shellRuntimeCreateContext();
}

void PSCALRuntimeDestroyShellContext(void *ctx) {
    if (!ctx) {
        return;
    }
    pthread_mutex_lock(&s_runtime_mutex);
    if (s_runtime_active && ctx == s_forced_shell_ctx) {
        pthread_mutex_unlock(&s_runtime_mutex);
        return;
    }
    if (ctx == s_forced_shell_ctx) {
        s_forced_shell_ctx = NULL;
        s_forced_shell_ctx_owned = false;
    }
    pthread_mutex_unlock(&s_runtime_mutex);
    shellRuntimeDestroyContext((ShellRuntimeState *)ctx);
}

void PSCALRuntimeSetShellContext(void *ctx, int takeOwnership) {
    pthread_mutex_lock(&s_runtime_mutex);
    ShellRuntimeState *incoming = (ShellRuntimeState *)ctx;
    if (s_forced_shell_ctx && s_forced_shell_ctx_owned && s_forced_shell_ctx != incoming &&
        !s_runtime_active) {
        shellRuntimeDestroyContext(s_forced_shell_ctx);
    }
    s_forced_shell_ctx = incoming;
    s_forced_shell_ctx_owned = (takeOwnership != 0);
    pthread_mutex_unlock(&s_runtime_mutex);
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
