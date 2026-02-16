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
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <setjmp.h>
#include <atomic>
#include <string>
#include <unordered_map>
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
    char *pscalRuntimeCopyMarketingVersion(void);
    void shellRuntimeInitSignals(void);
    ShellRuntimeState *shellRuntimeCreateContext(void);
    ShellRuntimeState *shellRuntimeActivateContext(ShellRuntimeState *ctx);
    void shellRuntimeDestroyContext(ShellRuntimeState *ctx);
    int32_t pscalRuntimeSetTabTitleForSession(uint64_t session_id, const char *title);
    int32_t pscalRuntimeSetTabStartupCommandForSession(uint64_t session_id, const char *command);
    void pscalRuntimeBindSessionToBootstrap(void *runtime_context, uint64_t session_id);
    void pscalRuntimeBindSessionToBootstrapHandle(void *bootstrap_handle, uint64_t session_id);
}

static const size_t kOutputBacklogLimit = 512 * 1024;
static const useconds_t kOutputBackpressureSleepUsec = 1000;
static const size_t kOutputRingCapacity = 512 * 1024;

static bool PSCALRuntimeOutputLoggingEnabled(void);
static bool PSCALRuntimeIODebugEnabled(void);
static void PSCALRuntimeDebugLogf(const char *format, ...);

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

struct PSCALRuntimeContext {
    PSCALRuntimeOutputHandler output_handler;
    PSCALRuntimeExitHandler exit_handler;
    void *handler_context;
    std::atomic<size_t> output_backlog;
    std::atomic<int> output_buffering_enabled;
    pthread_once_t output_ring_once;
    PSCALRuntimeOutputRing output_ring;
    bool output_ring_initialized;
    pthread_t runtime_thread_id;
    ShellRuntimeState *forced_shell_ctx;
    bool forced_shell_ctx_owned;
    pthread_mutex_t session_exit_mu;
    pthread_cond_t session_exit_cv;
    bool session_exit_pending;
    int session_exit_status;
    pthread_mutex_t runtime_mutex;
    bool runtime_active;
    bool runtime_launching;
    int master_fd;
    int input_fd;
    bool using_pscal_pty;
    uint64_t runtime_session_id;
    VProcSessionStdio *runtime_stdio;
    pthread_t output_thread;
    pthread_t runtime_thread;
    int pending_columns;
    int pending_rows;
    int pending_session_columns;
    int pending_session_rows;
    bool pending_session_winch;
    int runtime_log_fd;
    int script_capture_fd;
    std::string script_capture_path;
    std::atomic<int> output_log_enabled;
    pthread_mutex_t input_queue_mu;
    pthread_cond_t input_queue_cv;
    struct PSCALRuntimeInputChunk *input_queue_head;
    struct PSCALRuntimeInputChunk *input_queue_tail;
    size_t input_queue_bytes;
    pthread_once_t input_queue_once;
    pthread_t input_writer_thread;
    bool input_queue_shutdown;
    std::atomic<int> input_drop_logged;
};
typedef struct PSCALRuntimeContext PSCALRuntimeContext;

static void PSCALRuntimeContextInit(PSCALRuntimeContext *ctx) {
    if (!ctx) {
        return;
    }
    ctx->output_handler = NULL;
    ctx->exit_handler = NULL;
    ctx->handler_context = NULL;
    ctx->output_backlog.store(0, std::memory_order_relaxed);
    ctx->output_buffering_enabled.store(0, std::memory_order_relaxed);
    ctx->output_ring_once = PTHREAD_ONCE_INIT;
    memset(&ctx->output_ring, 0, sizeof(ctx->output_ring));
    ctx->output_ring_initialized = false;
    ctx->runtime_thread_id = 0;
    ctx->forced_shell_ctx = NULL;
    ctx->forced_shell_ctx_owned = false;
    pthread_mutex_init(&ctx->session_exit_mu, NULL);
    pthread_cond_init(&ctx->session_exit_cv, NULL);
    ctx->session_exit_pending = false;
    ctx->session_exit_status = 0;
    pthread_mutex_init(&ctx->runtime_mutex, NULL);
    ctx->runtime_active = false;
    ctx->runtime_launching = false;
    ctx->master_fd = -1;
    ctx->input_fd = -1;
    ctx->using_pscal_pty = false;
    ctx->runtime_session_id = 0;
    ctx->runtime_stdio = NULL;
    ctx->output_thread = 0;
    ctx->runtime_thread = 0;
    ctx->pending_columns = 0;
    ctx->pending_rows = 0;
    ctx->pending_session_columns = 0;
    ctx->pending_session_rows = 0;
    ctx->pending_session_winch = false;
    ctx->runtime_log_fd = -1;
    ctx->script_capture_fd = -1;
    ctx->script_capture_path.clear();
    ctx->output_log_enabled.store(-1, std::memory_order_relaxed);
    pthread_mutex_init(&ctx->input_queue_mu, NULL);
    pthread_cond_init(&ctx->input_queue_cv, NULL);
    ctx->input_queue_head = NULL;
    ctx->input_queue_tail = NULL;
    ctx->input_queue_bytes = 0;
    ctx->input_queue_once = PTHREAD_ONCE_INIT;
    ctx->input_writer_thread = 0;
    ctx->input_queue_shutdown = false;
    ctx->input_drop_logged.store(0, std::memory_order_relaxed);
}

static PSCALRuntimeContext s_runtime_context;
static pthread_once_t s_runtime_context_once = PTHREAD_ONCE_INIT;

static void PSCALRuntimeContextInitOnce(void) {
    PSCALRuntimeContextInit(&s_runtime_context);
}

static PSCALRuntimeContext *PSCALRuntimeSharedContext(void) {
    pthread_once(&s_runtime_context_once, PSCALRuntimeContextInitOnce);
    return &s_runtime_context;
}

static thread_local PSCALRuntimeContext *s_runtime_context_tls = nullptr;

static PSCALRuntimeContext *PSCALRuntimeCurrentContext(void) {
    PSCALRuntimeContext *ctx = s_runtime_context_tls;
    if (ctx) {
        return ctx;
    }
    return PSCALRuntimeSharedContext();
}

static PSCALRuntimeContext *PSCALRuntimeSetCurrentContext(PSCALRuntimeContext *ctx) {
    PSCALRuntimeContext *prev = s_runtime_context_tls;
    s_runtime_context_tls = ctx;
    return prev;
}

static void PSCALRuntimeBindContextOrLog(PSCALRuntimeContext *ctx, const char *thread_name) {
    if (!ctx) {
        static std::atomic<int> logged{0};
        if (logged.exchange(1, std::memory_order_relaxed) == 0) {
            PSCALRuntimeDebugLogf("PSCALRuntime: missing context bind on thread %s; using shared context (may stall other tabs)", thread_name ? thread_name : "unknown");
        }
        return;
    }
    PSCALRuntimeSetCurrentContext(ctx);
}

// TODO: Replace shared-context macros with explicit context plumbing for multi-session support.
#define s_output_handler (PSCALRuntimeCurrentContext()->output_handler)
#define s_exit_handler (PSCALRuntimeCurrentContext()->exit_handler)
#define s_handler_context (PSCALRuntimeCurrentContext()->handler_context)
#define s_output_backlog (PSCALRuntimeCurrentContext()->output_backlog)
#define s_output_buffering_enabled (PSCALRuntimeCurrentContext()->output_buffering_enabled)
#define s_output_ring_once (PSCALRuntimeCurrentContext()->output_ring_once)
#define s_output_ring (PSCALRuntimeCurrentContext()->output_ring)
#define s_output_ring_initialized (PSCALRuntimeCurrentContext()->output_ring_initialized)
#define s_runtime_thread_id (PSCALRuntimeCurrentContext()->runtime_thread_id)
#define s_forced_shell_ctx (PSCALRuntimeCurrentContext()->forced_shell_ctx)
#define s_forced_shell_ctx_owned (PSCALRuntimeCurrentContext()->forced_shell_ctx_owned)
#define s_session_exit_mu (PSCALRuntimeCurrentContext()->session_exit_mu)
#define s_session_exit_cv (PSCALRuntimeCurrentContext()->session_exit_cv)
#define s_session_exit_pending (PSCALRuntimeCurrentContext()->session_exit_pending)
#define s_session_exit_status (PSCALRuntimeCurrentContext()->session_exit_status)
#define s_runtime_mutex (PSCALRuntimeCurrentContext()->runtime_mutex)
#define s_runtime_active (PSCALRuntimeCurrentContext()->runtime_active)
#define s_runtime_launching (PSCALRuntimeCurrentContext()->runtime_launching)
#define s_master_fd (PSCALRuntimeCurrentContext()->master_fd)
#define s_input_fd (PSCALRuntimeCurrentContext()->input_fd)
#define s_using_pscal_pty (PSCALRuntimeCurrentContext()->using_pscal_pty)
#define s_runtime_session_id (PSCALRuntimeCurrentContext()->runtime_session_id)
#define s_runtime_stdio (PSCALRuntimeCurrentContext()->runtime_stdio)
#define s_output_thread (PSCALRuntimeCurrentContext()->output_thread)
#define s_runtime_thread (PSCALRuntimeCurrentContext()->runtime_thread)
#define s_pending_columns (PSCALRuntimeCurrentContext()->pending_columns)
#define s_pending_rows (PSCALRuntimeCurrentContext()->pending_rows)
#define s_pending_session_columns (PSCALRuntimeCurrentContext()->pending_session_columns)
#define s_pending_session_rows (PSCALRuntimeCurrentContext()->pending_session_rows)
#define s_pending_session_winch (PSCALRuntimeCurrentContext()->pending_session_winch)
#define s_runtime_log_fd (PSCALRuntimeCurrentContext()->runtime_log_fd)
#define s_script_capture_fd (PSCALRuntimeCurrentContext()->script_capture_fd)
#define s_script_capture_path (PSCALRuntimeCurrentContext()->script_capture_path)
#define s_output_log_enabled (PSCALRuntimeCurrentContext()->output_log_enabled)
#define s_input_queue_mu (PSCALRuntimeCurrentContext()->input_queue_mu)
#define s_input_queue_cv (PSCALRuntimeCurrentContext()->input_queue_cv)
#define s_input_queue_head (PSCALRuntimeCurrentContext()->input_queue_head)
#define s_input_queue_tail (PSCALRuntimeCurrentContext()->input_queue_tail)
#define s_input_queue_bytes (PSCALRuntimeCurrentContext()->input_queue_bytes)
#define s_input_queue_once (PSCALRuntimeCurrentContext()->input_queue_once)
#define s_input_writer_thread (PSCALRuntimeCurrentContext()->input_writer_thread)
#define s_input_queue_shutdown (PSCALRuntimeCurrentContext()->input_queue_shutdown)
#define s_input_drop_logged (PSCALRuntimeCurrentContext()->input_drop_logged)

static pthread_mutex_t s_session_context_mu = PTHREAD_MUTEX_INITIALIZER;
static std::unordered_map<uint64_t, PSCALRuntimeContext *> s_session_contexts;
static std::atomic<uint64_t> s_session_id_counter{1};

static PSCALRuntimeContext *PSCALRuntimeContextForSession(uint64_t session_id) {
    if (session_id == 0) {
        return NULL;
    }
    pthread_mutex_lock(&s_session_context_mu);
    auto it = s_session_contexts.find(session_id);
    PSCALRuntimeContext *ctx = (it != s_session_contexts.end()) ? it->second : NULL;
    pthread_mutex_unlock(&s_session_context_mu);
    return ctx;
}

static void PSCALRuntimeRegisterSessionContextInternal(uint64_t session_id, PSCALRuntimeContext *ctx) {
    if (!ctx || session_id == 0) {
        return;
    }
    pthread_mutex_lock(&s_session_context_mu);
    s_session_contexts[session_id] = ctx;
    pthread_mutex_unlock(&s_session_context_mu);
}

static void PSCALRuntimeUnregisterSessionContextInternal(uint64_t session_id, PSCALRuntimeContext *ctx) {
    if (session_id == 0) {
        return;
    }
    pthread_mutex_lock(&s_session_context_mu);
    auto it = s_session_contexts.find(session_id);
    if (it != s_session_contexts.end() && (!ctx || it->second == ctx)) {
        s_session_contexts.erase(it);
    }
    pthread_mutex_unlock(&s_session_context_mu);
}

uint64_t PSCALRuntimeNextSessionId(void) {
    return s_session_id_counter.fetch_add(1, std::memory_order_relaxed);
}

void PSCALRuntimeRegisterSessionContext(uint64_t session_id) {
    PSCALRuntimeContext *ctx = PSCALRuntimeCurrentContext();
    if (ctx) {
        pthread_mutex_lock(&ctx->runtime_mutex);
        ctx->runtime_session_id = session_id;
        pthread_mutex_unlock(&ctx->runtime_mutex);
    }
    if (PSCALRuntimeIODebugEnabled()) {
        PSCALRuntimeDebugLogf(
            "[runtime-io] register context session=%llu ctx=%p\n",
            (unsigned long long)session_id,
            (void *)ctx);
    }
    PSCALRuntimeRegisterSessionContextInternal(session_id, ctx);
}

void PSCALRuntimeUnregisterSessionContext(uint64_t session_id) {
    PSCALRuntimeUnregisterSessionContextInternal(session_id, NULL);
}

PSCALRuntimeContext *PSCALRuntimeCreateRuntimeContext(void) {
    PSCALRuntimeContext *ctx = (PSCALRuntimeContext *)calloc(1, sizeof(PSCALRuntimeContext));
    if (!ctx) {
        return NULL;
    }
    PSCALRuntimeContextInit(ctx);
    return ctx;
}

void PSCALRuntimeSetCurrentRuntimeContext(PSCALRuntimeContext *ctx) {
    PSCALRuntimeSetCurrentContext(ctx);
}

PSCALRuntimeContext *PSCALRuntimeGetCurrentRuntimeContext(void) {
    return PSCALRuntimeCurrentContext();
}

uint64_t PSCALRuntimeCurrentSessionId(void) {
    uint64_t session_id = 0;
    pthread_mutex_lock(&s_runtime_mutex);
    session_id = s_runtime_session_id;
    pthread_mutex_unlock(&s_runtime_mutex);
    return session_id;
}

VProcSessionStdio *PSCALRuntimeGetCurrentRuntimeStdio(void) {
    PSCALRuntimeContext *ctx = PSCALRuntimeCurrentContext();
    return ctx ? ctx->runtime_stdio : NULL;
}

void PSCALRuntimeSetCurrentRuntimeStdio(VProcSessionStdio *stdio_ctx) {
    PSCALRuntimeContext *ctx = PSCALRuntimeCurrentContext();
    if (ctx) {
        ctx->runtime_stdio = stdio_ctx;
    }
}

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
    if (PSCALRuntimeIODebugEnabled()) {
        PSCALRuntimeDebugLogf(
            "[runtime-io] register output handler session=%llu handler=%p ctx=%p\n",
            (unsigned long long)session_id,
            (void *)handler,
            context);
    }
    if (PSCALRuntimeOutputLoggingEnabled()) {
        NSLog(@"PSCALRuntime: register session output handler session=%llu handler=%p",
              (unsigned long long)session_id, (void *)handler);
    }
    vprocSessionSetOutputHandler(session_id, (VProcSessionOutputHandler)handler, context);
}

void PSCALRuntimeUnregisterSessionOutputHandler(uint64_t session_id) {
    if (session_id == 0) {
        return;
    }
    vprocSessionClearOutputHandler(session_id);
}

void PSCALRuntimeSetSessionOutputPaused(uint64_t session_id, int paused) {
    if (session_id == 0) {
        return;
    }
    vprocSessionSetOutputPaused(session_id, paused != 0);
}

extern "C" ShellRuntimeState *pscalRuntimeShellContextForSession(uint64_t session_id) {
    PSCALRuntimeContext *ctx = PSCALRuntimeContextForSession(session_id);
    if (!ctx) {
        return NULL;
    }
    PSCALRuntimeContext *prev_ctx = PSCALRuntimeSetCurrentContext(ctx);
    pthread_mutex_lock(&s_runtime_mutex);
    ShellRuntimeState *shell_ctx = s_forced_shell_ctx;
    pthread_mutex_unlock(&s_runtime_mutex);
    PSCALRuntimeSetCurrentContext(prev_ctx);
    return shell_ctx;
}

extern "C" void pscalRuntimeRegisterShellThread(uint64_t session_id, pthread_t tid) {
    PSCALRuntimeContext *ctx = PSCALRuntimeContextForSession(session_id);
    if (!ctx) {
        if (PSCALRuntimeIODebugEnabled()) {
            PSCALRuntimeDebugLogf("[runtime-io] register shell thread skipped session=%llu (no ctx)\n",
                                  (unsigned long long)session_id);
        }
        return;
    }
    PSCALRuntimeContext *prev_ctx = PSCALRuntimeSetCurrentContext(ctx);
    pscalRuntimeBindSessionToBootstrap((void *)ctx, session_id);
    if (s_handler_context) {
        pscalRuntimeBindSessionToBootstrapHandle(s_handler_context, session_id);
    }
    int pending_cols = 0;
    int pending_rows = 0;
    bool should_winch = false;
    pthread_mutex_lock(&s_runtime_mutex);
    if (session_id == s_runtime_session_id) {
        s_runtime_thread_id = tid;
        if (s_pending_session_columns > 0 && s_pending_session_rows > 0) {
            pending_cols = s_pending_session_columns;
            pending_rows = s_pending_session_rows;
            s_pending_session_columns = 0;
            s_pending_session_rows = 0;
            should_winch = true;
        } else if (s_pending_session_winch) {
            s_pending_session_winch = false;
            should_winch = true;
        }
    }
    pthread_mutex_unlock(&s_runtime_mutex);
    if (pending_cols > 0 && pending_rows > 0) {
        if (PSCALRuntimeSetSessionWinsize(session_id, pending_cols, pending_rows) != 0) {
            pthread_mutex_lock(&s_runtime_mutex);
            s_pending_session_columns = pending_cols;
            s_pending_session_rows = pending_rows;
            s_pending_session_winch = s_pending_session_winch || should_winch;
            pthread_mutex_unlock(&s_runtime_mutex);
            should_winch = false;
        }
    }
    if (PSCALRuntimeIODebugEnabled()) {
        PSCALRuntimeDebugLogf("[runtime-io] register shell thread session=%llu tid=%llu pending=%dx%d winch=%d\n",
                              (unsigned long long)session_id,
                              (unsigned long long)(uintptr_t)tid,
                              pending_cols,
                              pending_rows,
                              (int)should_winch);
    }
    if (should_winch) {
        (void)pthread_kill(tid, SIGWINCH);
    }
    PSCALRuntimeSetCurrentContext(prev_ctx);
}

extern "C" void pscalRuntimeKernelSessionExited(uint64_t session_id, int status) {
    PSCALRuntimeContext *ctx = PSCALRuntimeContextForSession(session_id);
    if (!ctx) {
        return;
    }
    PSCALRuntimeContext *prev_ctx = PSCALRuntimeSetCurrentContext(ctx);
    bool match = false;
    pthread_mutex_lock(&s_runtime_mutex);
    match = s_runtime_active && session_id == s_runtime_session_id;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (match) {
        pthread_mutex_lock(&s_session_exit_mu);
        s_session_exit_status = status;
        s_session_exit_pending = true;
        pthread_cond_broadcast(&s_session_exit_cv);
        pthread_mutex_unlock(&s_session_exit_mu);
    }
    PSCALRuntimeUnregisterSessionContextInternal(session_id, ctx);
    PSCALRuntimeSetCurrentContext(prev_ctx);
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
    s_output_ring_initialized = true;
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
        return false;
    }
    size_t space = s_output_ring.capacity > s_output_ring.size
        ? (s_output_ring.capacity - s_output_ring.size)
        : 0;
    if (length > space) {
        pthread_mutex_unlock(&s_output_ring.lock);
        return false;
    }
    size_t offset = 0;
    size_t tail = s_output_ring.tail;
    size_t first = s_output_ring.capacity - tail;
    if (first > length) {
        first = length;
    }
    memcpy(s_output_ring.data + tail, buffer + offset, first);
    tail = (tail + first) % s_output_ring.capacity;
    s_output_ring.size += first;
    offset += first;
    size_t second = length - first;
    if (second > 0) {
        memcpy(s_output_ring.data + tail, buffer + offset, second);
        tail = (tail + second) % s_output_ring.capacity;
        s_output_ring.size += second;
    }
    s_output_ring.tail = tail;
    pthread_cond_signal(&s_output_ring.can_read);
    pthread_mutex_unlock(&s_output_ring.lock);
    return true;
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

static bool PSCALRuntimeIODebugEnabled(void) {
    const char *env = getenv("PSCALI_IO_DEBUG");
    if (!env || env[0] == '\0' || strcmp(env, "0") == 0) {
        env = getenv("PSCALI_SSH_DEBUG");
    }
    return env && *env && strcmp(env, "0") != 0;
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
            NSLog(@"PSCALRuntime: output log path %@", path);
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

static void PSCALRuntimeDebugLogf(const char *format, ...) {
    if (!format) {
        return;
    }
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    size_t len = strlen(buf);
    if (len == 0) {
        return;
    }
    if (buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
    if (PSCALRuntimeOutputLoggingEnabled()) {
        PSCALRuntimeLogOutput(buf, strlen(buf));
        PSCALRuntimeLogOutput("\n", 1);
    } else {
        NSLog(@"%s", buf);
    }
}

static void PSCALRuntimeDirectOutputHandler(uint64_t session_id,
                                            const char *buffer,
                                            size_t length,
                                            void *context) {
    (void)context;
    if (!buffer || length == 0) {
        return;
    }
    PSCALRuntimeContext *session_ctx = PSCALRuntimeContextForSession(session_id);
    PSCALRuntimeContext *prev_ctx = NULL;
    if (session_ctx) {
        prev_ctx = PSCALRuntimeSetCurrentContext(session_ctx);
    } else {
        // No context registered for this session; avoid shared-ring fallback to
        // prevent cross-session backpressure. Dispatch directly to handlers.
        PSCALRuntimeDispatchOutput((const char *)buffer, length);
        return;
    }
    static std::atomic<int> logged{0};
    if (PSCALRuntimeOutputLoggingEnabled() && logged.exchange(1) == 0) {
        NSLog(@"PSCALRuntime: first direct output (%zu bytes)", length);
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
        if (session_ctx) {
            PSCALRuntimeSetCurrentContext(prev_ctx);
        }
        return;
    }
    // Avoid shunting non-main output into the shared ring if session context is missing.
    bool allow_queue = true;
    if (!session_ctx) {
        PSCALRuntimeContext *shared_ctx = PSCALRuntimeSharedContext();
        uint64_t shared_session_id = 0;
        if (shared_ctx) {
            pthread_mutex_lock(&shared_ctx->runtime_mutex);
            shared_session_id = shared_ctx->runtime_session_id;
            pthread_mutex_unlock(&shared_ctx->runtime_mutex);
        }
        if (session_id != 0 && session_id != shared_session_id) {
            allow_queue = false;
        }
    }
    if (allow_queue && PSCALRuntimeQueueOutput((const char *)buffer, length)) {
        if (session_ctx) {
            PSCALRuntimeSetCurrentContext(prev_ctx);
        }
        return;
    }
    PSCALRuntimeDispatchOutput((const char *)buffer, length);
    if (session_ctx) {
        PSCALRuntimeSetCurrentContext(prev_ctx);
    }
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
    if (PSCALRuntimeIODebugEnabled()) {
        PSCALRuntimeDebugLogf(
            "[runtime-io] write master session=%llu len=%zu\n",
            (unsigned long long)session_id,
            length);
    }
    int backoffMicros = 1000;
    while (true) {
        ssize_t wrote = vprocSessionWriteToMaster(session_id, data, length);
        if (wrote < 0) {
            if (PSCALRuntimeIODebugEnabled()) {
                PSCALRuntimeDebugLogf(
                    "[runtime-io] write master failed session=%llu errno=%d\n",
                    (unsigned long long)session_id,
                    errno);
            }
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

static ssize_t PSCALRuntimeWriteMasterNonBlocking(uint64_t session_id, const char *data, size_t length) {
    if (session_id == 0 || !data || length == 0) {
        return 0;
    }
    if (PSCALRuntimeIODebugEnabled()) {
        PSCALRuntimeDebugLogf(
            "[runtime-io] write master nb session=%llu len=%zu\n",
            (unsigned long long)session_id,
            length);
    }
    return vprocSessionWriteToMasterMode(session_id, data, length, false);
}

typedef enum {
    PSCALRuntimeInputKindFd = 0,
    PSCALRuntimeInputKindSession = 1
} PSCALRuntimeInputKind;

typedef struct PSCALRuntimeInputChunk {
    PSCALRuntimeInputKind kind;
    uint64_t session_id;
    int fd;
    char *data;
    size_t length;
    struct PSCALRuntimeInputChunk *next;
} PSCALRuntimeInputChunk;

static const size_t kInputQueueMaxBytes = 512 * 1024;

static void PSCALRuntimeClearInputQueueLocked(void) {
    PSCALRuntimeInputChunk *chunk = s_input_queue_head;
    s_input_queue_head = NULL;
    s_input_queue_tail = NULL;
    s_input_queue_bytes = 0;
    while (chunk) {
        PSCALRuntimeInputChunk *next = chunk->next;
        free(chunk->data);
        free(chunk);
        chunk = next;
    }
}

static void *PSCALRuntimeInputWriterThread(void *arg) {
    PSCALRuntimeContext *ctx = (PSCALRuntimeContext *)arg;
    PSCALRuntimeContext *prev_ctx = PSCALRuntimeCurrentContext();
    PSCALRuntimeBindContextOrLog(ctx, "pscal-input-writer");
    pthread_setname_np("pscal-input-writer");
    while (true) {
        pthread_mutex_lock(&s_input_queue_mu);
        while (!s_input_queue_head && !s_input_queue_shutdown) {
            pthread_cond_wait(&s_input_queue_cv, &s_input_queue_mu);
        }
        if (s_input_queue_shutdown && !s_input_queue_head) {
            pthread_mutex_unlock(&s_input_queue_mu);
            break;
        }
        if (!s_input_queue_head) {
            pthread_mutex_unlock(&s_input_queue_mu);
            continue;
        }
        PSCALRuntimeInputChunk *chunk = s_input_queue_head;
        s_input_queue_head = chunk->next;
        if (!s_input_queue_head) {
            s_input_queue_tail = NULL;
        }
        if (s_input_queue_bytes >= chunk->length) {
            s_input_queue_bytes -= chunk->length;
        } else {
            s_input_queue_bytes = 0;
        }
        pthread_mutex_unlock(&s_input_queue_mu);

        switch (chunk->kind) {
        case PSCALRuntimeInputKindSession:
            {
                ssize_t wrote = PSCALRuntimeWriteMasterNonBlocking(chunk->session_id, chunk->data, chunk->length);
                bool should_requeue = false;
                if (wrote < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        wrote = 0;
                        should_requeue = true;
                    }
                } else if ((size_t)wrote < chunk->length) {
                    should_requeue = true;
                }
                if (should_requeue) {
                    size_t remaining = chunk->length - (size_t)wrote;
                    if (remaining > 0) {
                        if (wrote > 0) {
                            memmove(chunk->data, chunk->data + wrote, remaining);
                        }
                        chunk->length = remaining;
                        chunk->next = NULL;
                        pthread_mutex_lock(&s_input_queue_mu);
                        if (!s_input_queue_shutdown &&
                            s_input_queue_bytes + remaining <= kInputQueueMaxBytes) {
                            if (s_input_queue_tail) {
                                s_input_queue_tail->next = chunk;
                            } else {
                                s_input_queue_head = chunk;
                            }
                            s_input_queue_tail = chunk;
                            s_input_queue_bytes += remaining;
                            pthread_cond_signal(&s_input_queue_cv);
                            pthread_mutex_unlock(&s_input_queue_mu);
                            chunk = NULL;
                            usleep(2000);
                        } else {
                            pthread_mutex_unlock(&s_input_queue_mu);
                        }
                    }
                }
            }
            break;
        case PSCALRuntimeInputKindFd:
            PSCALRuntimeWriteWithBackoff(chunk->fd, chunk->data, chunk->length);
            break;
        }

        if (chunk) {
            free(chunk->data);
            free(chunk);
        }
    }
    PSCALRuntimeSetCurrentContext(prev_ctx);
    return NULL;
}

static void PSCALRuntimeInputQueueInitOnce(void) {
    PSCALRuntimeContext *ctx = PSCALRuntimeCurrentContext();
    int err = pthread_create(&ctx->input_writer_thread, NULL, PSCALRuntimeInputWriterThread, ctx);
    if (err != 0) {
        ctx->input_writer_thread = 0;
    }
}

static bool PSCALRuntimeEnqueueInput(PSCALRuntimeInputKind kind,
                                     uint64_t session_id,
                                     int fd,
                                     const char *data,
                                     size_t length) {
    if (!data || length == 0) {
        return false;
    }
    if (kind == PSCALRuntimeInputKindSession && session_id == 0) {
        return false;
    }
    if (kind == PSCALRuntimeInputKindFd && fd < 0) {
        return false;
    }
    pthread_once(&s_input_queue_once, PSCALRuntimeInputQueueInitOnce);
    if (!s_input_writer_thread) {
        return false;
    }
    PSCALRuntimeInputChunk *chunk = (PSCALRuntimeInputChunk *)calloc(1, sizeof(PSCALRuntimeInputChunk));
    if (!chunk) {
        return false;
    }
    char *copy = (char *)malloc(length);
    if (!copy) {
        free(chunk);
        return false;
    }
    memcpy(copy, data, length);
    chunk->kind = kind;
    chunk->session_id = session_id;
    chunk->fd = fd;
    chunk->data = copy;
    chunk->length = length;
    chunk->next = NULL;

    pthread_mutex_lock(&s_input_queue_mu);
    if (s_input_queue_bytes + length > kInputQueueMaxBytes) {
        pthread_mutex_unlock(&s_input_queue_mu);
        free(copy);
        free(chunk);
        if (s_input_drop_logged.fetch_add(1, std::memory_order_relaxed) == 0) {
            PSCALRuntimeLogLine("PSCALRuntime: input queue full; dropping input");
        }
        return false;
    }
    if (s_input_queue_tail) {
        s_input_queue_tail->next = chunk;
    } else {
        s_input_queue_head = chunk;
    }
    s_input_queue_tail = chunk;
    s_input_queue_bytes += length;
    pthread_cond_signal(&s_input_queue_cv);
    pthread_mutex_unlock(&s_input_queue_mu);
    return true;
}

void PSCALRuntimeDestroyRuntimeContext(PSCALRuntimeContext *ctx) {
    if (!ctx || ctx == PSCALRuntimeSharedContext()) {
        return;
    }
    PSCALRuntimeContext *prev_ctx = PSCALRuntimeSetCurrentContext(ctx);

    pthread_mutex_lock(&s_runtime_mutex);
    bool active = s_runtime_active;
    uint64_t session_id = s_runtime_session_id;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (active) {
        NSLog(@"PSCALRuntime: refusing to destroy active runtime context");
        PSCALRuntimeSetCurrentContext(prev_ctx);
        return;
    }
    if (session_id != 0) {
        PSCALRuntimeUnregisterSessionContextInternal(session_id, ctx);
    }

    pthread_mutex_lock(&s_input_queue_mu);
    s_input_queue_shutdown = true;
    PSCALRuntimeClearInputQueueLocked();
    pthread_cond_broadcast(&s_input_queue_cv);
    pthread_mutex_unlock(&s_input_queue_mu);

    if (s_input_writer_thread) {
        pthread_join(s_input_writer_thread, NULL);
        s_input_writer_thread = 0;
    }

    PSCALRuntimeEndScriptCapture();
    if (s_runtime_log_fd >= 0) {
        close(s_runtime_log_fd);
        s_runtime_log_fd = -1;
    }
    if (s_output_ring.data) {
        free(s_output_ring.data);
        s_output_ring.data = NULL;
    }
    s_output_ring.active = false;
    s_output_ring.size = 0;

    if (s_output_ring_initialized) {
        pthread_mutex_destroy(&s_output_ring.lock);
        pthread_cond_destroy(&s_output_ring.can_read);
        pthread_cond_destroy(&s_output_ring.can_write);
    }
    pthread_mutex_destroy(&s_runtime_mutex);
    pthread_mutex_destroy(&s_session_exit_mu);
    pthread_cond_destroy(&s_session_exit_cv);
    pthread_mutex_destroy(&s_input_queue_mu);
    pthread_cond_destroy(&s_input_queue_cv);

    PSCALRuntimeSetCurrentContext(prev_ctx);
    free(ctx);
}

static void PSCALRuntimeWaitForReadable(int fd) {
    if (fd < 0) {
        return;
    }
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int res = 0;
    do {
        res = poll(&pfd, 1, 10);
    } while (res < 0 && errno == EINTR);
}

static void *PSCALRuntimeOutputPump(void *arg) {
    PSCALRuntimeContext *ctx = (PSCALRuntimeContext *)arg;
    PSCALRuntimeContext *prev_ctx = PSCALRuntimeCurrentContext();
    PSCALRuntimeBindContextOrLog(ctx, "pscal-output-pump");
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
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    PSCALRuntimeWaitForReadable(fd);
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                PSCALRuntimeWaitForReadable(fd);
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
    PSCALRuntimeSetCurrentContext(prev_ctx);
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
    if (s_runtime_active || s_runtime_launching) {
        pthread_mutex_unlock(&s_runtime_mutex);
        NSLog(@"PSCALRuntime: launch aborted (already running)");
        errno = EBUSY;
        return -1;
    }
    s_runtime_launching = true;
    PSCALRuntimeEnsurePendingWindowSizeLocked();

    const int initial_columns = s_pending_columns;
    const int initial_rows = s_pending_rows;
    pthread_mutex_lock(&s_session_exit_mu);
    s_session_exit_pending = false;
    s_session_exit_status = 0;
    pthread_mutex_unlock(&s_session_exit_mu);
    pthread_mutex_unlock(&s_runtime_mutex);

    (void)vprocEnsureKernelPid();
    const uint64_t session_id = PSCALRuntimeNextSessionId();
    pthread_mutex_lock(&s_runtime_mutex);
    s_runtime_active = true;
    s_runtime_launching = false;
    s_runtime_session_id = session_id;
    s_runtime_thread = pthread_self();
    s_runtime_thread_id = 0;
    pthread_mutex_unlock(&s_runtime_mutex);
    pscalRuntimeBindSessionToBootstrap((void *)PSCALRuntimeCurrentContext(), session_id);
    if (s_handler_context) {
        pscalRuntimeBindSessionToBootstrapHandle(s_handler_context, session_id);
    }
    PSCALRuntimeRegisterSessionContext(session_id);
    PSCALRuntimeRegisterSessionOutputHandler(session_id, PSCALRuntimeDirectOutputHandler, NULL);
    int master_fd = -1;
    int input_fd = -1;
    if (PSCALRuntimeCreateShellSession(argc, argv, session_id, &master_fd, &input_fd) != 0) {
        pthread_mutex_lock(&s_runtime_mutex);
        s_runtime_active = false;
        s_runtime_launching = false;
        s_runtime_session_id = 0;
        memset(&s_runtime_thread, 0, sizeof(s_runtime_thread));
        memset(&s_runtime_thread_id, 0, sizeof(s_runtime_thread_id));
        pthread_mutex_unlock(&s_runtime_mutex);
        PSCALRuntimeUnregisterSessionOutputHandler(session_id);
        PSCALRuntimeUnregisterSessionContext(session_id);
        return -1;
    }

    pthread_mutex_lock(&s_runtime_mutex);
    s_master_fd = -1;
    s_input_fd = -1;
    s_using_pscal_pty = true;
    s_runtime_stdio = NULL;
    pthread_mutex_unlock(&s_runtime_mutex);
    s_output_backlog.store(0, std::memory_order_relaxed);
    if (PSCALRuntimeOutputBufferingEnabled()) {
        PSCALRuntimeOutputRingActivate();
    }

    PSCALRuntimeSetSessionWinsize(session_id, initial_columns, initial_rows);
    // Ensure session-heavy tools (e.g., embedded Editor) write into a
    // writable sandbox directory instead of the app bundle.
    @autoreleasepool {
        // Prefer a stable Documents/home working directory so tools like Editor
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

    memset(&s_output_thread, 0, sizeof(s_output_thread));

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
    s_master_fd = -1;
    s_input_fd = -1;
    s_using_pscal_pty = false;
    s_runtime_session_id = 0;
    s_runtime_stdio = NULL;
    s_runtime_active = false;
    s_runtime_launching = false;
    memset(&s_runtime_thread, 0, sizeof(s_runtime_thread));
    memset(&s_runtime_thread_id, 0, sizeof(s_runtime_thread_id));
    pthread_mutex_unlock(&s_runtime_mutex);

    if (pump_fd >= 0) {
        close(pump_fd);
    }
    if (stdin_fd >= 0 && stdin_fd != pump_fd) {
        close(stdin_fd);
    }
    PSCALRuntimeUnregisterSessionOutputHandler(session_id);
    if (PSCALRuntimeOutputBufferingEnabled()) {
        PSCALRuntimeOutputRingDeactivate();
    }
    PSCALRuntimeEndScriptCapture();
    if (s_runtime_log_fd >= 0) {
        close(s_runtime_log_fd);
        s_runtime_log_fd = -1;
    }
    PSCALRuntimeUnregisterSessionContext(session_id);

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
    PSCALRuntimeContext *ctx;
    int result;
} PSCALRuntimeThreadContext;

static void *PSCALRuntimeThreadMain(void *arg) {
    PSCALRuntimeThreadContext *context = (PSCALRuntimeThreadContext *)arg;
    PSCALRuntimeContext *prev_ctx = PSCALRuntimeCurrentContext();
    PSCALRuntimeBindContextOrLog(context->ctx, "pscal-runtime");
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
    PSCALRuntimeSetCurrentContext(prev_ctx);
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
        .ctx = PSCALRuntimeCurrentContext(),
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
    const uint64_t session_id = s_runtime_session_id;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (session_id != 0) {
        PSCALRuntimeEnqueueInput(PSCALRuntimeInputKindSession, session_id, -1, utf8, length);
        return;
    }
    if (fd < 0) {
        return;
    }
    PSCALRuntimeEnqueueInput(PSCALRuntimeInputKindFd, 0, fd, utf8, length);
}

void PSCALRuntimeSendInputForSession(uint64_t session_id, const char *utf8, size_t length) {
    if (!utf8 || length == 0 || session_id == 0) {
        return;
    }
    if (PSCALRuntimeIODebugEnabled()) {
        PSCALRuntimeDebugLogf(
            "[runtime-io] enqueue input session=%llu len=%zu\n",
            (unsigned long long)session_id,
            length);
    }
    PSCALRuntimeEnqueueInput(PSCALRuntimeInputKindSession, session_id, -1, utf8, length);
}

void PSCALRuntimeSendInputUrgent(uint64_t session_id, const char *utf8, size_t length) {
    if (!utf8 || length == 0 || session_id == 0) {
        return;
    }
    ssize_t wrote = PSCALRuntimeWriteMasterNonBlocking(session_id, utf8, length);
    if (wrote < 0 && PSCALRuntimeIODebugEnabled()) {
        PSCALRuntimeDebugLogf(
            "[runtime-io] urgent input write failed session=%llu errno=%d\n",
            (unsigned long long)session_id,
            errno);
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
    pthread_t runtime_thread = s_runtime_thread_id;
    bool using_pscal_pty = s_using_pscal_pty;
    uint64_t session_id = s_runtime_session_id;
    pthread_mutex_unlock(&s_runtime_mutex);

    if (using_pscal_pty && session_id != 0) {
        PSCALRuntimeSetSessionWinsize(session_id, columns, rows);
    } else {
        PSCALRuntimeApplyWindowSize(fd, columns, rows);
    }

    if (active && runtime_thread) {
        (void)pthread_kill(runtime_thread, SIGWINCH);
    }
}

int PSCALRuntimeSetTabTitle(const char *title) {
    uint64_t session_id = PSCALRuntimeCurrentSessionId();
    if (session_id == 0 || !title) {
        return -1;
    }
    return (int)pscalRuntimeSetTabTitleForSession(session_id, title);
}

int PSCALRuntimeSetTabStartupCommand(const char *command) {
    uint64_t session_id = PSCALRuntimeCurrentSessionId();
    if (session_id == 0 || !command) {
        return -1;
    }
    return (int)pscalRuntimeSetTabStartupCommandForSession(session_id, command);
}

void PSCALRuntimeUpdateSessionWindowSize(uint64_t session_id, int columns, int rows) {
    if (session_id == 0 || columns <= 0 || rows <= 0) {
        return;
    }
    PSCALRuntimeContext *ctx = PSCALRuntimeContextForSession(session_id);
    if (!ctx) {
        if (PSCALRuntimeIODebugEnabled()) {
            PSCALRuntimeDebugLogf("[runtime-io] winsize session=%llu %dx%d (no ctx)\n",
                                  (unsigned long long)session_id,
                                  columns,
                                  rows);
        }
        (void)PSCALRuntimeSetSessionWinsize(session_id, columns, rows);
        return;
    }
    int rc = PSCALRuntimeSetSessionWinsize(session_id, columns, rows);
    PSCALRuntimeContext *prev_ctx = PSCALRuntimeSetCurrentContext(ctx);
    pthread_mutex_lock(&s_runtime_mutex);
    if (rc == 0) {
        s_pending_session_columns = 0;
        s_pending_session_rows = 0;
        if (s_runtime_thread_id) {
            pthread_t target = s_runtime_thread_id;
            pthread_mutex_unlock(&s_runtime_mutex);
            if (PSCALRuntimeIODebugEnabled()) {
                PSCALRuntimeDebugLogf("[runtime-io] winsize session=%llu %dx%d rc=%d signal tid=%llu\n",
                                      (unsigned long long)session_id,
                                      columns,
                                      rows,
                                      rc,
                                      (unsigned long long)(uintptr_t)target);
            }
            (void)pthread_kill(target, SIGWINCH);
            PSCALRuntimeSetCurrentContext(prev_ctx);
            return;
        }
        s_pending_session_winch = true;
    } else {
        s_pending_session_columns = columns;
        s_pending_session_rows = rows;
        s_pending_session_winch = true;
    }
    if (PSCALRuntimeIODebugEnabled()) {
        PSCALRuntimeDebugLogf("[runtime-io] winsize session=%llu %dx%d rc=%d tid=%llu pending_winch=%d\n",
                              (unsigned long long)session_id,
                              columns,
                              rows,
                              rc,
                              (unsigned long long)(uintptr_t)s_runtime_thread_id,
                              (int)s_pending_session_winch);
    }
    pthread_mutex_unlock(&s_runtime_mutex);
    PSCALRuntimeSetCurrentContext(prev_ctx);
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

int PSCALRuntimeSendSignalForSession(uint64_t session_id, int signo) {
    if (session_id == 0) {
        return 0;
    }
    int sig = 0;
    switch (signo) {
        case SIGINT:
            sig = SIGINT;
            break;
        case SIGTSTP:
            sig = SIGTSTP;
            break;
        case SIGTERM:
            sig = SIGINT;
            break;
        default:
            return 0;
    }

    if (vprocRequestControlSignalForSession(session_id, sig)) {
        return 1;
    }

    /* Do not fall back to shell-scoped virtual dispatch here. If a
     * session-targeted control request cannot be delivered, callers can choose
     * literal control-byte fallback (required for remote/ssh semantics). */
    return 0;
}

void PSCALRuntimeSendSignal(int signo) {
    pthread_mutex_lock(&s_runtime_mutex);
    bool active = s_runtime_active;
    uint64_t session_id = s_runtime_session_id;
    int shell_pid_hint = (s_runtime_stdio) ? s_runtime_stdio->shell_pid : 0;
    pthread_t target = s_runtime_thread_id;
    pthread_mutex_unlock(&s_runtime_mutex);
    if (!active && session_id == 0 && shell_pid_hint <= 0) {
        return;
    }
    auto requestSignal = [&](int sig) -> bool {
        if (shell_pid_hint > 0 && vprocRequestControlSignalForShell(shell_pid_hint, sig)) {
            return true;
        }
        return vprocRequestControlSignal(sig);
    };
    auto softSignalingDisabled = [&]() -> bool {
        const char *flag = getenv("PSCALI_DISABLE_SOFT_SIGNALING");
        return flag && flag[0] != '\0' && strcmp(flag, "0") != 0;
    };
    switch (signo) {
        case SIGINT:
            if (session_id != 0) {
                if (PSCALRuntimeSendSignalForSession(session_id, SIGINT)) {
                    return;
                }
                if (softSignalingDisabled()) {
                    return;
                }
                if (!requestSignal(SIGINT)) {
                    pscalRuntimeRequestSigint();
                }
                return;
            }
            if (!requestSignal(SIGINT)) {
                pscalRuntimeRequestSigint();
            }
            return;
        case SIGTSTP:
            if (session_id != 0) {
                if (PSCALRuntimeSendSignalForSession(session_id, SIGTSTP)) {
                    return;
                }
                if (softSignalingDisabled()) {
                    return;
                }
                if (!requestSignal(SIGTSTP)) {
                    pscalRuntimeRequestSigtstp();
                }
                return;
            }
            if (!requestSignal(SIGTSTP)) {
                pscalRuntimeRequestSigtstp();
            }
            return;
        case SIGTERM:
            /* Runtime restarts should request a virtual interrupt instead of
             * delivering host SIGTERM/SIGINT into the app process. */
            if (session_id != 0 && PSCALRuntimeSendSignalForSession(session_id, SIGINT)) {
                return;
            }
            if (!requestSignal(SIGINT)) {
                pscalRuntimeRequestSigint();
            }
            return;
        case SIGWINCH:
            if (target != 0) {
                (void)pthread_kill(target, SIGWINCH);
            }
            return;
        default:
            return;
    }
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

void PSCALRuntimeRegisterLocationReaderObserver(PSCALLocationReaderObserver cb, void *context) {
    vprocLocationDeviceRegisterReaderObserver(cb, context);
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
