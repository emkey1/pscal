//
//  sdl.c
//  Pscal
//
//  Created by Michael Miller on 5/7/25.
//
#ifdef SDL
#include "core/sdl_headers.h"
#include PSCALI_SDL_HEADER
#include PSCALI_SDL_IMAGE_HEADER
#include PSCALI_SDL_TTF_HEADER
#include PSCALI_SDL_MIXER_HEADER
#if PSCALI_HAS_SYSWM
#include PSCALI_SDL_SYSWM_HEADER
#endif
// Include audio.h directly (declares MAX_SOUNDS and gLoadedSounds)
#include "audio.h"

#include "core/utils.h"
#include "vm/vm.h"
#include "sdl_ios_dispatch.h"
static void sdlLogEvent(const char *label, const SDL_Event *event);
#if defined(PSCAL_TARGET_IOS)
#include <dispatch/dispatch.h>
#include <pthread.h>
static inline void sdlRunOnMainThreadSync(void (^block)(void)) {
    if (pthread_main_np() != 0) {
        block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), block);
    }
}

void sdlFlushSpuriousQuitEvents(void) {
    SDL_FlushEvent(SDL_QUIT);
    SDL_FlushEvent(SDL_APP_TERMINATING);
    SDL_FlushEvent(SDL_APP_WILLENTERBACKGROUND);
    SDL_FlushEvent(SDL_APP_DIDENTERBACKGROUND);
}
#else
void sdlFlushSpuriousQuitEvents(void) { }
#endif

#ifdef DEBUG
static const char* sdlDescribeEventType(Uint32 type) {
    switch (type) {
        case SDL_QUIT: return "SDL_QUIT";
        case SDL_APP_TERMINATING: return "SDL_APP_TERMINATING";
        case SDL_APP_WILLENTERBACKGROUND: return "SDL_APP_WILLENTERBACKGROUND";
        case SDL_APP_DIDENTERBACKGROUND: return "SDL_APP_DIDENTERBACKGROUND";
        case SDL_APP_WILLENTERFOREGROUND: return "SDL_APP_WILLENTERFOREGROUND";
        case SDL_APP_DIDENTERFOREGROUND: return "SDL_APP_DIDENTERFOREGROUND";
        case SDL_KEYDOWN: return "SDL_KEYDOWN";
        case SDL_KEYUP: return "SDL_KEYUP";
        case SDL_MOUSEBUTTONDOWN: return "SDL_MOUSEBUTTONDOWN";
        case SDL_MOUSEBUTTONUP: return "SDL_MOUSEBUTTONUP";
        case SDL_MOUSEMOTION: return "SDL_MOUSEMOTION";
        case SDL_WINDOWEVENT: return "SDL_WINDOWEVENT";
        default: break;
    }
    return "UNKNOWN";
}

static void sdlDumpEvent(const char *label, const SDL_Event *event) {
    if (!label || !event) {
        return;
    }
    SDL_DEBUG_LOG("%s: %s (%u)", label, sdlDescribeEventType(event->type), event->type);
}
#else
#define sdlDumpEvent(label, event) ((void)0)
#endif

#ifdef DEBUG
static void sdlLogEvent(const char *label, const SDL_Event *event) {
    if (!label || !event) {
        return;
    }
    SDL_DEBUG_LOG("%s: %s (%u)", label, sdlDescribeEventType(event->type), event->type);
}
#else
static void sdlLogEvent(const char *label, const SDL_Event *event) {
    (void)label;
    (void)event;
}
#endif

#include "pscal_sdl_runtime.h" // Includes SDL family headers and SDL declarations
#include "Pascal/globals.h" // Includes SDL.h and SDL_ttf.h via its includes, and audio.h

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef DEBUG
static SDL_SpinLock gSdlDebugLogLock = 0;
static FILE* gSdlDebugLogFile = NULL;
static bool gSdlDebugLogInitialized = false;
static char gSdlDebugLogPath[PATH_MAX];

static void sdlDebugEnsureDirectoryForPath(const char* path) {
    if (!path) {
        return;
    }
    const char* lastSlash = strrchr(path, '/');
    if (!lastSlash) {
        return;
    }

    size_t dirLen = (size_t)(lastSlash - path);
    char directory[PATH_MAX];
    if (dirLen == 0 || dirLen >= sizeof(directory)) {
        return;
    }
    memcpy(directory, path, dirLen);
    directory[dirLen] = '\0';
    mkdir(directory, 0755);
}

static const char* sdlDebugResolveLogPath(void) {
    if (gSdlDebugLogPath[0] != '\0') {
        return gSdlDebugLogPath;
    }

    const char* overridePath = getenv("PSCAL_SDL_LOG_PATH");
    if (overridePath && *overridePath) {
        snprintf(gSdlDebugLogPath, sizeof(gSdlDebugLogPath), "%s", overridePath);
        return gSdlDebugLogPath;
    }

    const char* home = getenv("HOME");
    if (home && *home) {
        snprintf(gSdlDebugLogPath, sizeof(gSdlDebugLogPath), "%s/Documents/pscal_sdl_log.txt", home);
    } else {
        snprintf(gSdlDebugLogPath, sizeof(gSdlDebugLogPath), "/tmp/pscal_sdl_log.txt");
    }

    return gSdlDebugLogPath;
}

static FILE* sdlDebugOpenLogFile(void) {
    if (gSdlDebugLogInitialized) {
        return gSdlDebugLogFile;
    }

    gSdlDebugLogInitialized = true;
    const char* logPath = sdlDebugResolveLogPath();
    if (!logPath || *logPath == '\0') {
        return NULL;
    }

    sdlDebugEnsureDirectoryForPath(logPath);
    gSdlDebugLogFile = fopen(logPath, "a");
    if (!gSdlDebugLogFile) {
        return NULL;
    }

    setvbuf(gSdlDebugLogFile, NULL, _IOLBF, 0);
    fprintf(gSdlDebugLogFile, "\n==== SDL debug log started (pid=%d) ===\n", (int)getpid());
    fflush(gSdlDebugLogFile);
    return gSdlDebugLogFile;
}

static void sdlDebugLogf(const char* fmt, ...) {
    FILE* logFile = sdlDebugOpenLogFile();
    if (!logFile) {
        return;
    }

    SDL_AtomicLock(&gSdlDebugLogLock);
    va_list args;
    va_start(args, fmt);
    vfprintf(logFile, fmt, args);
    va_end(args);
    fflush(logFile);
    SDL_AtomicUnlock(&gSdlDebugLogLock);
}

static void sdlLogBreakRequestedChange(const char* reason, int newValue) {
    sdlDebugLogf("[BREAK] %s -> %d\n", reason ? reason : "(unknown)", newValue);
}

#define SDL_DEBUG_LOG(fmt, ...) sdlDebugLogf("[DEBUG SDL] " fmt, ##__VA_ARGS__)
#define SDL_DEBUG_SET_BREAK_REQUESTED(value, reason) \
    do { \
        atomic_store(&break_requested, (value)); \
        sdlLogBreakRequestedChange((reason), (value)); \
    } while (0)
#else
#define sdlDebugLogf(...) ((void)0)
#define sdlLogBreakRequestedChange(...) ((void)0)
#define SDL_DEBUG_LOG(...) ((void)0)
#define SDL_DEBUG_SET_BREAK_REQUESTED(value, reason) \
    atomic_store(&break_requested, (value))
#endif

#ifdef ENABLE_EXT_BUILTIN_3D
extern void cleanupBalls3DRenderingResources(void);
#else
static void cleanupBalls3DRenderingResources(void) {}
#endif

#ifdef SDL_VIDEO_DRIVER_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#endif

// SDL Global Variable Definitions
SDL_Window* gSdlWindow = NULL;
SDL_Renderer* gSdlRenderer = NULL;
SDL_GLContext gSdlGLContext = NULL;
SDL_Color gSdlCurrentColor = { 255, 255, 255, 255 }; // Default white
bool gSdlInitialized = false;
int gSdlWidth = 0;
int gSdlHeight = 0;
TTF_Font* gSdlFont = NULL;
int gSdlFontSize   = 16;
SDL_Texture* gSdlTextures[MAX_SDL_TEXTURES];
int gSdlTextureWidths[MAX_SDL_TEXTURES];
int gSdlTextureHeights[MAX_SDL_TEXTURES];
int gSdlTextureAccesses[MAX_SDL_TEXTURES];
bool gSdlTtfInitialized = false;
bool gSdlImageInitialized = false; // Tracks if IMG_Init was called for PNG/JPG etc.

static bool gSdlInputWatchInstalled = false;

#ifdef SDL_VIDEO_DRIVER_X11
static Atom gX11WmPingAtom = None;
static bool gX11WmAtomsInitialized = false;
#endif

#define MAX_PENDING_KEYCODES 128

static SDL_Keycode gPendingKeycodes[MAX_PENDING_KEYCODES];
static int gPendingKeyStart = 0;
static int gPendingKeyCount = 0;
static const int kTextureAccessInvalid = -1;

#if defined(PSCALI_SDL3)
static bool isWindowCloseEvent(const SDL_Event* event) {
    return event && event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED;
}
#else
static bool isWindowCloseEvent(const SDL_Event* event) {
    return event && event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_CLOSE;
}
#endif

bool sdlTextInputActive(void) {
#if defined(PSCALI_SDL3)
    if (!gSdlWindow) {
        return false;
    }
    return SDL_TextInputActive(gSdlWindow);
#else
    return SDL_IsTextInputActive();
#endif
}

void sdlStartTextInput(void) {
#if defined(PSCALI_SDL3)
    if (gSdlWindow) {
        SDL_StartTextInput(gSdlWindow);
    }
#else
    SDL_StartTextInput();
#endif
}

void sdlStopTextInput(void) {
#if defined(PSCAL_TARGET_IOS)
    sdlRunOnMainThreadSync(^{
#if defined(PSCALI_SDL3)
        if (gSdlWindow) {
            SDL_StopTextInput(gSdlWindow);
        }
#else
        SDL_StopTextInput();
#endif
    });
#else
#if defined(PSCALI_SDL3)
    if (gSdlWindow) {
        SDL_StopTextInput(gSdlWindow);
    }
#else
    SDL_StopTextInput();
#endif
#endif
}

static void applyWindowBorderOffsets(int* winX, int* winY) {
    if (!winX || !winY) {
        return;
    }
#if defined(PSCALI_SDL3)
    if (gSdlWindow) {
        int borderTop = 0, borderLeft = 0, borderBottom = 0, borderRight = 0;
        if (SDL_GetWindowBordersSize(gSdlWindow,
                                     &borderTop,
                                     &borderLeft,
                                     &borderBottom,
                                     &borderRight)) {
            *winX += borderLeft;
            *winY += borderTop;
        }
    }
#elif defined(SDL_VERSION_ATLEAST)
#if SDL_VERSION_ATLEAST(2,0,5)
    if (gSdlWindow) {
        int borderTop = 0, borderLeft = 0, borderBottom = 0, borderRight = 0;
        if (SDL_GetWindowBordersSize(gSdlWindow,
                                     &borderTop,
                                     &borderLeft,
                                     &borderBottom,
                                     &borderRight) == 0) {
            *winX += borderLeft;
            *winY += borderTop;
        }
    }
#endif
#endif
}

static bool isPrintableKeycode(SDL_Keycode code) {
    return code >= 32 && code <= 126;
}

static void enqueuePendingKeycode(SDL_Keycode code) {
    if (gPendingKeyCount == MAX_PENDING_KEYCODES) {
        gPendingKeyStart = (gPendingKeyStart + 1) % MAX_PENDING_KEYCODES;
        gPendingKeyCount--;
    }

    int tail = (gPendingKeyStart + gPendingKeyCount) % MAX_PENDING_KEYCODES;
    gPendingKeycodes[tail] = code;
    gPendingKeyCount++;
}

static void enqueueUtf8Text(const char* text) {
    if (!text || *text == '\0') {
        return;
    }

    const unsigned char* bytes = (const unsigned char*)text;
    size_t length = strlen(text);
    size_t index = 0;

    while (index < length) {
        unsigned char byte = bytes[index];
        Uint32 codepoint = 0;
        size_t advance = 1;

        if ((byte & 0x80u) == 0) {
            codepoint = byte;
        } else if ((byte & 0xE0u) == 0xC0u && index + 1 < length) {
            unsigned char byte2 = bytes[index + 1];
            if ((byte2 & 0xC0u) != 0x80u) {
                index += advance;
                continue;
            }
            codepoint = ((Uint32)(byte & 0x1Fu) << 6) | (Uint32)(byte2 & 0x3Fu);
            advance = 2;
        } else if ((byte & 0xF0u) == 0xE0u && index + 2 < length) {
            unsigned char byte2 = bytes[index + 1];
            unsigned char byte3 = bytes[index + 2];
            if (((byte2 & 0xC0u) != 0x80u) || ((byte3 & 0xC0u) != 0x80u)) {
                index += advance;
                continue;
            }
            codepoint = ((Uint32)(byte & 0x0Fu) << 12) |
                ((Uint32)(byte2 & 0x3Fu) << 6) |
                (Uint32)(byte3 & 0x3Fu);
            advance = 3;
        } else if ((byte & 0xF8u) == 0xF0u && index + 3 < length) {
            unsigned char byte2 = bytes[index + 1];
            unsigned char byte3 = bytes[index + 2];
            unsigned char byte4 = bytes[index + 3];
            if (((byte2 & 0xC0u) != 0x80u) ||
                ((byte3 & 0xC0u) != 0x80u) ||
                ((byte4 & 0xC0u) != 0x80u)) {
                index += advance;
                continue;
            }
            codepoint = ((Uint32)(byte & 0x07u) << 18) |
                ((Uint32)(byte2 & 0x3Fu) << 12) |
                ((Uint32)(byte3 & 0x3Fu) << 6) |
                (Uint32)(byte4 & 0x3Fu);
            advance = 4;
        } else {
            index += advance;
            continue;
        }

        if (codepoint <= 0x10FFFFu) {
            enqueuePendingKeycode((SDL_Keycode)codepoint);
        }

        index += advance;
    }
}

#if PSCALI_HAS_SYSWM
static void handleSysWmEvent(const SDL_Event* event) {
#ifdef SDL_VIDEO_DRIVER_X11
    if (!event || event->type != SDL_SYSWMEVENT) {
        return;
    }

    SDL_SysWMmsg* msg = event->syswm.msg;
    if (!msg || msg->subsystem != SDL_SYSWM_X11) {
        return;
    }

    XEvent* xevent = &msg->msg.x11.event;
    if (!xevent || xevent->type != ClientMessage) {
        return;
    }

    Display* display = xevent->xclient.display;
    if (!display) {
        return;
    }

    if (!gX11WmAtomsInitialized) {
        gX11WmPingAtom = XInternAtom(display, "_NET_WM_PING", False);
        gX11WmAtomsInitialized = true;
    }

    if (gX11WmPingAtom == None) {
        return;
    }

    if ((Atom)xevent->xclient.message_type != gX11WmPingAtom) {
        return;
    }

    Window clientWindow = xevent->xclient.window;

    XEvent reply = *xevent;
    reply.xclient.window = DefaultRootWindow(display);
    reply.xclient.data.l[1] = clientWindow;

    XSendEvent(display, reply.xclient.window, False,
        SubstructureNotifyMask | SubstructureRedirectMask, &reply);
    XFlush(display);
#else
    (void)event;
#endif
}
#else
static void handleSysWmEvent(const SDL_Event* event) {
    (void)event;
}
#endif

static bool dequeuePendingKeycode(SDL_Keycode* outCode) {
    if (gPendingKeyCount == 0) {
        return false;
    }

    if (outCode) {
        *outCode = gPendingKeycodes[gPendingKeyStart];
    }

    gPendingKeyStart = (gPendingKeyStart + 1) % MAX_PENDING_KEYCODES;
    gPendingKeyCount--;
    return true;
}

static bool hasPendingKeycode(void) {
    return gPendingKeyCount > 0;
}

static void resetPendingKeycodes(void) {
    gPendingKeyStart = 0;
    gPendingKeyCount = 0;
}

static int sdlInputWatch(void* userdata, SDL_Event* event) {
    (void)userdata;

    if (!event) {
        return 0;
    }

#if PSCALI_HAS_SYSWM
    if (event->type == SDL_SYSWMEVENT) {
        sdlLogEvent("EventWatch", event);
        handleSysWmEvent(event);
        return 0;
    }
#endif

    sdlLogEvent("EventWatch", event);
    if (event->type == SDL_QUIT) {
        SDL_DEBUG_SET_BREAK_REQUESTED(1, "EventWatch SDL_QUIT");
    } else if (event->type == SDL_KEYDOWN) {
        SDL_Keycode sym = event->key.keysym.sym;
        if (sym == SDLK_ESCAPE || sym == SDLK_q) {
            SDL_DEBUG_SET_BREAK_REQUESTED(1, "EventWatch hotkey");
        }
    }

    return 0;
}

static SDL_Scancode resolveScancodeFromName(const char* name) {
    if (!name) {
        return SDL_SCANCODE_UNKNOWN;
    }

    char compact[64];
    size_t srcLen = strlen(name);
    if (srcLen >= sizeof(compact)) {
        srcLen = sizeof(compact) - 1;
    }
    size_t compactLen = 0;
    for (size_t i = 0; i < srcLen && compactLen < sizeof(compact) - 1; ++i) {
        char ch = name[i];
        if (ch != ' ' && ch != '_' && ch != '-') {
            compact[compactLen++] = (char)tolower((unsigned char)ch);
        }
    }
    compact[compactLen] = '\0';

    if (strcmp(compact, "lshift") == 0 || strcmp(compact, "leftshift") == 0) {
        return SDL_SCANCODE_LSHIFT;
    }
    if (strcmp(compact, "rshift") == 0 || strcmp(compact, "rightshift") == 0) {
        return SDL_SCANCODE_RSHIFT;
    }
    if (strcmp(compact, "shift") == 0) {
        return SDL_SCANCODE_LSHIFT;
    }
    if (strcmp(compact, "space") == 0) {
        return SDL_SCANCODE_SPACE;
    }
    if (strcmp(compact, "escape") == 0 || strcmp(compact, "esc") == 0) {
        return SDL_SCANCODE_ESCAPE;
    }
    if (strcmp(compact, "enter") == 0 || strcmp(compact, "return") == 0) {
        return SDL_SCANCODE_RETURN;
    }
    if (strcmp(compact, "minus") == 0 || strcmp(compact, "hyphen") == 0) {
        return SDL_SCANCODE_MINUS;
    }
    if (strcmp(compact, "equals") == 0 || strcmp(compact, "equal") == 0) {
        return SDL_SCANCODE_EQUALS;
    }

    char normalized[64];
    size_t normLen = srcLen;
    if (normLen >= sizeof(normalized)) {
        normLen = sizeof(normalized) - 1;
    }
    for (size_t i = 0; i < normLen; ++i) {
        char ch = name[i];
        if (ch == '_' || ch == '-') {
            normalized[i] = ' ';
        } else {
            normalized[i] = ch;
        }
    }
    normalized[normLen] = '\0';

    if (normLen == 1) {
        char letter[2];
        letter[0] = (char)toupper((unsigned char)normalized[0]);
        letter[1] = '\0';
        SDL_Keycode keycode = SDL_GetKeyFromName(letter);
        if (keycode != SDLK_UNKNOWN) {
            SDL_Scancode sc = SDL_GetScancodeFromKey(keycode);
            if (sc != SDL_SCANCODE_UNKNOWN) {
                return sc;
            }
        }
    }

    SDL_Keycode keycode = SDL_GetKeyFromName(normalized);
    if (keycode != SDLK_UNKNOWN) {
        SDL_Scancode sc = SDL_GetScancodeFromKey(keycode);
        if (sc != SDL_SCANCODE_UNKNOWN) {
            return sc;
        }
    }

    return SDL_GetScancodeFromName(normalized);
}

static SDL_Scancode resolveScancodeFromValue(Value arg, bool* out_ok) {
    if (out_ok) {
        *out_ok = false;
    }

    if (arg.type == TYPE_STRING && arg.s_val != NULL) {
        SDL_Scancode sc = resolveScancodeFromName(arg.s_val);
        if (sc != SDL_SCANCODE_UNKNOWN && out_ok) {
            *out_ok = true;
        }
        return sc;
    }

    if (IS_INTLIKE(arg)) {
        long long raw = AS_INTEGER(arg);
        if (raw >= 0 && raw < SDL_NUM_SCANCODES) {
            if (out_ok) {
                *out_ok = true;
            }
            return (SDL_Scancode)raw;
        }

        SDL_Scancode sc = SDL_GetScancodeFromKey((SDL_Keycode)raw);
        if (sc != SDL_SCANCODE_UNKNOWN) {
            if (out_ok) {
                *out_ok = true;
            }
            return sc;
        }
        return SDL_SCANCODE_UNKNOWN;
    }

    return SDL_SCANCODE_UNKNOWN;
}

void initializeTextureSystem(void) {
    for (int i = 0; i < MAX_SDL_TEXTURES; ++i) {
        gSdlTextures[i] = NULL;
        gSdlTextureWidths[i] = 0;
        gSdlTextureHeights[i] = 0;
        gSdlTextureAccesses[i] = kTextureAccessInvalid;
    }
}

void sdlEnsureInputWatch(void) {
    if (!gSdlInitialized) {
        return;
    }

    if (!gSdlInputWatchInstalled) {
        SDL_AddEventWatch(sdlInputWatch, NULL);
        gSdlInputWatchInstalled = true;
    }
}

static void cleanupSdlWindowResourcesInternal(void) {
    resetPendingKeycodes();

    if (gSdlInitialized && sdlTextInputActive()) {
        sdlStopTextInput();
    }

    if (gSdlGLContext) {
        cleanupBalls3DRenderingResources();
        SDL_GL_DeleteContext(gSdlGLContext);
        gSdlGLContext = NULL;
        #ifdef DEBUG
        SDL_DEBUG_LOG("cleanupSdlWindowResources: SDL_GL_DeleteContext successful.\n");
        #endif
    }
    if (gSdlRenderer) {
        SDL_DestroyRenderer(gSdlRenderer);
        gSdlRenderer = NULL;
        #ifdef DEBUG
        SDL_DEBUG_LOG("cleanupSdlWindowResources: SDL_DestroyRenderer successful.\n");
        #endif
    }
    if (gSdlWindow) {
        SDL_DestroyWindow(gSdlWindow);
        gSdlWindow = NULL;
        #ifdef DEBUG
        SDL_DEBUG_LOG("cleanupSdlWindowResources: SDL_DestroyWindow successful.\n");
        #endif
    }
    gSdlWidth = 0;
    gSdlHeight = 0;
    gSdlCurrentColor.r = 255;
    gSdlCurrentColor.g = 255;
    gSdlCurrentColor.b = 255;
    gSdlCurrentColor.a = 255;
}

void cleanupSdlWindowResources(void) {
#if defined(PSCAL_TARGET_IOS)
    sdlRunOnMainThreadSync(^{
        cleanupSdlWindowResourcesInternal();
    });
#else
    cleanupSdlWindowResourcesInternal();
#endif
}

// Helper to find a free texture slot or return an error ID
int findFreeTextureID(void) {
    for (int i = 0; i < MAX_SDL_TEXTURES; ++i) {
        if (gSdlTextures[i] == NULL) {
            return i;
        }
    }
    return -1; // No free slots
}

static void sdlCleanupAtExitInternal(void) {
#ifdef DEBUG
    SDL_DEBUG_LOG("Running sdlCleanupAtExit (Final Program Exit Cleanup)...\n");
#endif

    // --- Clean up SDL_ttf resources ---
    if (gSdlFont) {
        TTF_CloseFont(gSdlFont);
        gSdlFont = NULL;
        #ifdef DEBUG
        SDL_DEBUG_LOG("sdlCleanupAtExit: TTF_CloseFont successful.\n");
        #endif
    }
    if (gSdlTtfInitialized) {
        TTF_Quit();
        gSdlTtfInitialized = false;
        #ifdef DEBUG
        SDL_DEBUG_LOG("sdlCleanupAtExit: TTF_Quit successful.\n");
        #endif
    }

    // --- Clean up SDL_image resources --- // <<< NEW SECTION
    if (gSdlImageInitialized) {
        IMG_Quit();
        gSdlImageInitialized = false;
        #ifdef DEBUG
        SDL_DEBUG_LOG("sdlCleanupAtExit: IMG_Quit successful.\n");
        #endif
    }
    // --- END NEW SECTION ---


    // --- Clean up SDL_mixer audio resources ---
    for (int i = 0; i < MAX_SOUNDS; ++i) {
        if (gLoadedSounds[i] != NULL) {
            Mix_FreeChunk(gLoadedSounds[i]);
            gLoadedSounds[i] = NULL;
            DEBUG_PRINT("[DEBUG AUDIO] sdlCleanupAtExit: Auto-freed sound chunk at index %d.\n", i);
        }
    }
    int open_freq, open_channels;
    Uint16 open_format;
    if (Mix_QuerySpec(&open_freq, &open_format, &open_channels) != 0) {
        Mix_CloseAudio();
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG AUDIO] sdlCleanupAtExit: Mix_CloseAudio successful.\n");
        #endif
    } else {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG AUDIO] sdlCleanupAtExit: Mix_CloseAudio skipped (audio not open or already closed by audioQuitSystem).\n");
        #endif
    }
    Mix_Quit();
    #ifdef DEBUG
    fprintf(stderr, "[DEBUG AUDIO] sdlCleanupAtExit: Mix_Quit successful.\n");
    #endif
    gSoundSystemInitialized = false;


    // --- Clean up core SDL video and timer resources ---
    // Destroy textures first
/*    for (int i = 0; i < MAX_SDL_TEXTURES; ++i) {
        if (gSdlTextures[i] != NULL) {
            SDL_DestroyTexture(gSdlTextures[i]);
            gSdlTextures[i] = NULL;
        }
    }
 */
    if (gSdlInputWatchInstalled) {
        SDL_DelEventWatch(sdlInputWatch, NULL);
        gSdlInputWatchInstalled = false;
    }

    cleanupSdlWindowResourcesInternal();
    if (gSdlInitialized) {
        SDL_Quit();
        gSdlInitialized = false;
        #ifdef DEBUG
        SDL_DEBUG_LOG("sdlCleanupAtExit: SDL_Quit successful.\n");
        #endif
    }

#ifdef DEBUG
    SDL_DEBUG_LOG("sdlCleanupAtExit finished.\n");
#endif
}

void sdlCleanupAtExit(void) {
#if defined(PSCAL_TARGET_IOS)
    sdlRunOnMainThreadSync(^{
        sdlCleanupAtExitInternal();
    });
#else
    sdlCleanupAtExitInternal();
#endif
}


PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinInitgraph) {
    if (arg_count != 3 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) || args[2].type != TYPE_STRING) {
        runtimeError(vm, "VM Error: InitGraph expects (Integer, Integer, String)");
        return makeVoid();
    }

    if (!gSdlInitialized) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
            runtimeError(vm, "Runtime error: SDL_Init failed in InitGraph: %s", SDL_GetError());
            return makeVoid();
        }
        gSdlInitialized = true;

        // Allow clicks to focus the window on macOS and avoid dropped initial events
#ifdef SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
#endif
#if PSCALI_HAS_SYSWM
        SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#endif
    }

    cleanupSdlWindowResources();

    int width = (int)AS_INTEGER(args[0]);
    int height = (int)AS_INTEGER(args[1]);
    const char* title = args[2].s_val ? args[2].s_val : "Pscal Graphics";

    if (width <= 0 || height <= 0) {
        runtimeError(vm, "Runtime error: InitGraph width and height must be positive.");
        return makeVoid();
    }
    
    gSdlWindow = NULL;
#if defined(PSCALI_SDL3)
    gSdlWindow = SDL_CreateWindow(title, width, height, SDL_WINDOW_SHOWN);
    if (gSdlWindow) {
        SDL_SetWindowPosition(gSdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
#else
    gSdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_SHOWN);
#endif
    if (!gSdlWindow) {
        runtimeError(vm, "Runtime error: SDL_CreateWindow failed: %s", SDL_GetError());
        return makeVoid();
    }

    gSdlWidth = width;
    gSdlHeight = height;

    gSdlRenderer = NULL;
#if defined(PSCALI_SDL3)
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, NULL);
    if (gSdlRenderer) {
        SDL_SetRenderVSync(gSdlRenderer, 1);
    }
#else
    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#endif
    if (!gSdlRenderer) {
        runtimeError(vm, "Runtime error: SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL;
        return makeVoid();
    }

    gSdlGLContext = NULL;
    initializeTextureSystem();

    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gSdlRenderer);
    SDL_RenderPresent(gSdlRenderer);
    SDL_PumpEvents(); // Process any pending events so the window becomes visible
    SDL_RaiseWindow(gSdlWindow); // Ensure the window appears in the foreground
#if SDL_VERSION_ATLEAST(2,0,5)
#ifndef PSCALI_SDL3
    SDL_SetWindowInputFocus(gSdlWindow); // Request focus for the new window
#endif
#endif

    gSdlCurrentColor.r = 255; gSdlCurrentColor.g = 255; gSdlCurrentColor.b = 255; gSdlCurrentColor.a = 255;

    sdlEnsureInputWatch();
    sdlFlushSpuriousQuitEvents();

    if (!sdlTextInputActive()) {
        sdlStartTextInput();
    }

    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinClosegraph) {
    if (arg_count != 0) runtimeError(vm, "CloseGraph expects 0 arguments.");
    cleanupSdlWindowResources();
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinFillrect) {
    if (arg_count != 4) { runtimeError(vm, "FillRect expects 4 integer arguments."); return makeVoid(); }
    // ... type checks for all 4 args being integer ...
    SDL_Rect rect;
    rect.x = (int)AS_INTEGER(args[0]);
    rect.y = (int)AS_INTEGER(args[1]);
    rect.w = (int)AS_INTEGER(args[2]) - rect.x + 1;
    rect.h = (int)AS_INTEGER(args[3]) - rect.y + 1;
    if (rect.w < 0) { rect.x += rect.w; rect.w = -rect.w; }
    if (rect.h < 0) { rect.y += rect.h; rect.h = -rect.h; }
    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    SDL_RenderFillRect(gSdlRenderer, &rect);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinUpdatetexture) {
    if (arg_count != 2) {
        runtimeError(vm, "UpdateTexture expects 2 arguments (TextureID: Integer; PixelData: ARRAY OF Byte).");
        return makeVoid();
    }

    Value idVal = args[0];
    Value pixelDataVal = args[1];

    if (!isIntlikeType(idVal.type) || pixelDataVal.type != TYPE_ARRAY) {
        runtimeError(vm, "UpdateTexture argument type mismatch.");
        return makeVoid();
    }
    if (pixelDataVal.element_type != TYPE_BYTE) {
        runtimeError(vm, "UpdateTexture PixelData must be an ARRAY OF Byte. Got array of %s.", varTypeToString(pixelDataVal.element_type));
        return makeVoid();
    }

    int textureID = (int)AS_INTEGER(idVal);
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        runtimeError(vm, "UpdateTexture called with invalid TextureID %d.", textureID);
        return makeVoid();
    }

    int texWidth = gSdlTextureWidths[textureID];
    int pitch = texWidth * 4; // 4 bytes per pixel for RGBA8888
    int expectedPscalArraySize = texWidth * gSdlTextureHeights[textureID] * 4;
    int pscalArrayTotalElements = calculateArrayTotalSize(&pixelDataVal);

    if (pscalArrayTotalElements != expectedPscalArraySize) {
        runtimeError(vm, "UpdateTexture PixelData array size (%d) does not match texture dimensions*BPP (%d).",
                pscalArrayTotalElements, expectedPscalArraySize);
        return makeVoid();
    }

    // Since the VM passes direct Value pointers, we can create a temporary C buffer from them.
    unsigned char* c_pixel_buffer = (unsigned char*)malloc(expectedPscalArraySize);
    if (!c_pixel_buffer) {
        runtimeError(vm, "Failed to allocate C buffer for UpdateTexture.");
        return makeVoid();
    }

    if (arrayUsesPackedBytes(&pixelDataVal)) {
        if (!pixelDataVal.array_raw) {
            free(c_pixel_buffer);
            runtimeError(vm, "UpdateTexture PixelData buffer is NULL.");
            return makeVoid();
        }
        memcpy(c_pixel_buffer, pixelDataVal.array_raw, (size_t)expectedPscalArraySize);
    } else {
        for (int i = 0; i < expectedPscalArraySize; ++i) {
            c_pixel_buffer[i] = (unsigned char)AS_INTEGER(pixelDataVal.array_val[i]);
        }
    }

#if defined(PSCALI_SDL3)
    if (!SDL_UpdateTexture(gSdlTextures[textureID], NULL, c_pixel_buffer, pitch)) {
#else
    if (SDL_UpdateTexture(gSdlTextures[textureID], NULL, c_pixel_buffer, pitch) != 0) {
#endif
        runtimeError(vm, "SDL_UpdateTexture failed: %s", SDL_GetError());
    }

    free(c_pixel_buffer);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinUpdatescreen) {
    if (arg_count != 0) runtimeError(vm, "UpdateScreen expects 0 arguments.");
    if (gSdlRenderer) SDL_RenderPresent(gSdlRenderer);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinCleardevice) {
    if (arg_count != 0) {
        runtimeError(vm, "Runtime error: ClearDevice expects 0 arguments.");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        runtimeError(vm, "Runtime error: Graphics mode not initialized before ClearDevice.");
        return makeVoid();
    }
    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
    SDL_RenderClear(gSdlRenderer);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGetmaxx) {
    if (arg_count != 0) runtimeError(vm, "GetMaxX expects 0 arguments.");
    return makeInt(gSdlWidth > 0 ? gSdlWidth - 1 : 0);
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGetmaxy) {
    if (arg_count != 0) runtimeError(vm, "GetMaxY expects 0 arguments.");
    return makeInt(gSdlHeight > 0 ? gSdlHeight - 1 : 0);
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGetticks) {
    if (arg_count != 0) {
        runtimeError(vm, "GetTicks expects 0 arguments.");
        return makeInt(0);
    }
    return makeInt((long long)PSCAL_SDL_GET_TICKS());
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGetscreensize) {
    if (arg_count != 2) {
        runtimeError(vm, "GetScreenSize expects 2 arguments.");
        return makeBoolean(false);
    }
    if (args[0].type != TYPE_POINTER || args[1].type != TYPE_POINTER) {
        runtimeError(vm, "GetScreenSize requires VAR parameters, but a non-pointer type was received.");
        return makeBoolean(false);
    }

    Value* width_ptr = (Value*)args[0].ptr_val;
    Value* height_ptr = (Value*)args[1].ptr_val;

    if (!width_ptr || !height_ptr) {
        runtimeError(vm, "GetScreenSize received a NIL pointer for a VAR parameter.");
        return makeBoolean(false);
    }

    int width = 0;
    int height = 0;

    if (gSdlInitialized && gSdlWindow) {
        width = gSdlWidth;
        height = gSdlHeight;

        int currentWidth = 0;
        int currentHeight = 0;
        SDL_GetWindowSize(gSdlWindow, &currentWidth, &currentHeight);
        if (currentWidth > 0) {
            width = currentWidth;
        }
        if (currentHeight > 0) {
            height = currentHeight;
        }

        if (width > 0) {
            gSdlWidth = width;
        }
        if (height > 0) {
            gSdlHeight = height;
        }
    } else {
#if defined(PSCAL_TARGET_IOS)
        /* On iOS SDL forbids initializing video from background threads before main
         * has run. After SDL_main this branch is not used because gSdlWindow is set. */
        width = gSdlWidth > 0 ? gSdlWidth : 1024;
        height = gSdlHeight > 0 ? gSdlHeight : 768;
        return makeBoolean(true);
#else
        bool initialized_video = false;
        Uint32 was_init = PSCAL_SDL_WAS_INIT(SDL_INIT_VIDEO);
        if ((was_init & SDL_INIT_VIDEO) == 0) {
            if (PSCAL_SDL_INIT_SUBSYSTEM(SDL_INIT_VIDEO) != 0) {
                runtimeError(vm, "Unable to initialize SDL video subsystem for GetScreenSize: %s", SDL_GetError());
                return makeBoolean(false);
            }
            initialized_video = true;
        }

#if defined(PSCALI_SDL3)
        SDL_DisplayID displayId = SDL_GetPrimaryDisplay();
        const SDL_DisplayMode* mode = NULL;
        if (displayId) {
            mode = SDL_GetDesktopDisplayMode(displayId);
            if (!mode) {
                mode = SDL_GetCurrentDisplayMode(displayId);
            }
        }
        if (!mode || mode->w <= 0 || mode->h <= 0) {
            if (initialized_video) {
                PSCAL_SDL_QUIT_SUBSYSTEM(SDL_INIT_VIDEO);
            }
            runtimeError(vm, "Unable to query display size for GetScreenSize: %s", SDL_GetError());
            return makeBoolean(false);
        }
        width = mode->w;
        height = mode->h;
#else
        SDL_DisplayMode mode;
        int display_status = SDL_GetDesktopDisplayMode(0, &mode);
        if (display_status != 0 || mode.w <= 0 || mode.h <= 0) {
            display_status = SDL_GetCurrentDisplayMode(0, &mode);
        }

        if (display_status != 0 || mode.w <= 0 || mode.h <= 0) {
            if (initialized_video) {
                PSCAL_SDL_QUIT_SUBSYSTEM(SDL_INIT_VIDEO);
            }
            runtimeError(vm, "Unable to query display size for GetScreenSize: %s", SDL_GetError());
            return makeBoolean(false);
        }

        width = mode.w;
        height = mode.h;
#endif

        if (initialized_video) {
            PSCAL_SDL_QUIT_SUBSYSTEM(SDL_INIT_VIDEO);
        }
#endif
    }

    freeValue(width_ptr);
    *width_ptr = makeInt(width);
    freeValue(height_ptr);
    *height_ptr = makeInt(height);

    return makeBoolean(true);
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinSetrgbcolor) {
    if (arg_count != 3) { runtimeError(vm, "SetRGBColor expects 3 arguments."); return makeVoid(); }
    gSdlCurrentColor.r = (Uint8)AS_INTEGER(args[0]);
    gSdlCurrentColor.g = (Uint8)AS_INTEGER(args[1]);
    gSdlCurrentColor.b = (Uint8)AS_INTEGER(args[2]);
    gSdlCurrentColor.a = 255;
    if (gSdlRenderer) {
        SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    }
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinQuittextsystem) {
    if (arg_count != 0) runtimeError(vm, "QuitTextSystem expects 0 arguments.");
    if (gSdlFont) { TTF_CloseFont(gSdlFont); gSdlFont = NULL; }
    if (gSdlTtfInitialized) { TTF_Quit(); gSdlTtfInitialized = false; }
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGettextsize) {
    if (arg_count != 3) { runtimeError(vm, "GetTextSize expects 3 arguments."); return makeVoid(); }
    if (!gSdlFont) { runtimeError(vm, "Font not initialized for GetTextSize."); return makeVoid(); }
    
    const char* text = AS_STRING(args[0]);
    Value* width_ptr = (Value*)args[1].ptr_val;
    Value* height_ptr = (Value*)args[2].ptr_val;
    
    int w, h;
    TTF_SizeUTF8(gSdlFont, text, &w, &h);
    
    freeValue(width_ptr); *width_ptr = makeInt(w);
    freeValue(height_ptr); *height_ptr = makeInt(h);
    
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGetmousestate) {
    if (arg_count != 3 && arg_count != 4) {
        runtimeError(vm, "GetMouseState expects 3 or 4 arguments.");
        return makeVoid();
    }

    // --- ADD SAFETY CHECKS ---
    if (args[0].type != TYPE_POINTER || args[1].type != TYPE_POINTER || args[2].type != TYPE_POINTER) {
        runtimeError(vm, "GetMouseState requires VAR parameters, but a non-pointer type was received.");
        return makeVoid();
    }

    Value* x_ptr = (Value*)args[0].ptr_val;
    Value* y_ptr = (Value*)args[1].ptr_val;
    Value* buttons_ptr = (Value*)args[2].ptr_val;
    Value* inside_ptr = NULL;

    if (arg_count == 4) {
        if (args[3].type != TYPE_POINTER) {
            runtimeError(vm, "GetMouseState requires VAR parameters, but a non-pointer type was received.");
            return makeVoid();
        }
        inside_ptr = (Value*)args[3].ptr_val;
        if (!inside_ptr) {
            runtimeError(vm, "GetMouseState received a NIL pointer for a VAR parameter.");
            return makeVoid();
        }
    }

    if (!x_ptr || !y_ptr || !buttons_ptr) {
        runtimeError(vm, "GetMouseState received a NIL pointer for a VAR parameter.");
        return makeVoid();
    }
    // --- END SAFETY CHECKS ---

    if (!gSdlInitialized || !gSdlWindow) {
        runtimeError(vm, "Graphics system not initialized for GetMouseState.");
        return makeVoid();
    }

    int mse_x = 0, mse_y = 0;
    Uint32 sdl_buttons = 0;
    bool inside_window = false;
    bool has_focus = false;

#ifdef __APPLE__
    // On macOS avoid SDL_PumpEvents from non-main threads. Use global mouse
    // state and convert to window-relative coordinates with border and HiDPI
    // adjustments.
    int global_x = 0, global_y = 0;
    sdl_buttons = SDL_GetGlobalMouseState(&global_x, &global_y);

    int win_x = 0, win_y = 0;
    SDL_GetWindowPosition(gSdlWindow, &win_x, &win_y);
    applyWindowBorderOffsets(&win_x, &win_y);
    mse_x = global_x - win_x;
    mse_y = global_y - win_y;
    int win_w = 0, win_h = 0; SDL_GetWindowSize(gSdlWindow, &win_w, &win_h);
    inside_window = (global_x >= win_x && global_x < (win_x + win_w) &&
                     global_y >= win_y && global_y < (win_y + win_h));
    has_focus = (SDL_GetMouseFocus() == gSdlWindow);

    // Do NOT scale to renderer output size here; the rest of the VM and
    // examples operate in window pixel coordinates (texture and drawing use
    // window dimensions), so returning window-relative coordinates preserves
    // 1:1 mapping with drawing code even on HiDPI displays.

    if (mse_x < 0) {
        mse_x = 0;
    }
    if (mse_y < 0) {
        mse_y = 0;
    }
    if (gSdlWidth > 0 && mse_x >= gSdlWidth) mse_x = gSdlWidth - 1;
    if (gSdlHeight > 0 && mse_y >= gSdlHeight) mse_y = gSdlHeight - 1;
#else
    // Update SDL state and prefer window-relative coordinates when we have focus.
    SDL_PumpEvents();
    SDL_Window* focus_window = SDL_GetMouseFocus();
    if (focus_window == gSdlWindow) {
        sdl_buttons = SDL_GetMouseState(&mse_x, &mse_y);
        int win_w = 0, win_h = 0; SDL_GetWindowSize(gSdlWindow, &win_w, &win_h);
        inside_window = (mse_x >= 0 && mse_x < win_w && mse_y >= 0 && mse_y < win_h);
        has_focus = true;
    } else {
        int global_x = 0, global_y = 0;
        sdl_buttons = SDL_GetGlobalMouseState(&global_x, &global_y);
        int win_x = 0, win_y = 0; SDL_GetWindowPosition(gSdlWindow, &win_x, &win_y);
        mse_x = global_x - win_x;
        mse_y = global_y - win_y;
        int win_w = 0, win_h = 0; SDL_GetWindowSize(gSdlWindow, &win_w, &win_h);
        inside_window = (global_x >= win_x && global_x < (win_x + win_w) &&
                         global_y >= win_y && global_y < (win_y + win_h));
        has_focus = false;
        
        if (mse_x < 0) {
            mse_x = 0;
        }
        if (mse_y < 0) {
            mse_y = 0;
        }
        if (gSdlWidth > 0 && mse_x >= gSdlWidth) mse_x = gSdlWidth - 1;
        if (gSdlHeight > 0 && mse_y >= gSdlHeight) mse_y = gSdlHeight - 1;
    }
#endif

    // Ignore buttons if the window is not focused or the pointer is outside it.
    if (gSdlWindow) {
        Uint32 windowFlags = SDL_GetWindowFlags(gSdlWindow);
        if (!inside_window && (windowFlags & SDL_WINDOW_MOUSE_FOCUS)) {
            inside_window = true;
        }
        if (!has_focus && (windowFlags & SDL_WINDOW_INPUT_FOCUS)) {
            has_focus = true;
        }
    }

    if (!inside_window) {
        sdl_buttons = 0;
    }
    
    int pscal_buttons = 0;
    if (sdl_buttons & SDL_BUTTON_LMASK) pscal_buttons |= 1;
    if (sdl_buttons & SDL_BUTTON_MMASK) pscal_buttons |= 2;
    if (sdl_buttons & SDL_BUTTON_RMASK) pscal_buttons |= 4;

    // Safely assign the retrieved values
    freeValue(x_ptr);
    *x_ptr = makeInt(mse_x);

    freeValue(y_ptr);
    *y_ptr = makeInt(mse_y);

    freeValue(buttons_ptr);
    *buttons_ptr = makeInt(pscal_buttons);

    if (inside_ptr) {
        freeValue(inside_ptr);
        *inside_ptr = makeInt((inside_window && has_focus) ? 1 : 0);
    }

    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinDestroytexture) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) { runtimeError(vm, "DestroyTexture expects 1 integer argument."); return makeVoid(); }
    int textureID = (int)AS_INTEGER(args[0]);
    if (textureID >= 0 && textureID < MAX_SDL_TEXTURES && gSdlTextures[textureID]) {
        SDL_DestroyTexture(gSdlTextures[textureID]);
        gSdlTextures[textureID] = NULL;
        gSdlTextureWidths[textureID] = 0;
        gSdlTextureHeights[textureID] = 0;
        gSdlTextureAccesses[textureID] = kTextureAccessInvalid;
    }
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinRendercopyrect) {
    if (arg_count != 5) {
        fprintf(stderr, "Runtime error: RenderCopyRect expects 5 arguments.\n");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics not initialized before RenderCopyRect.\n");
        return makeVoid();
    }
    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) || !IS_INTLIKE(args[2]) ||
        !IS_INTLIKE(args[3]) || !IS_INTLIKE(args[4])) {
        fprintf(stderr, "Runtime error: RenderCopyRect expects integer arguments (TextureID, DestX, DestY, DestW, DestH).\n");
        return makeVoid();
    }
    int textureID = (int)AS_INTEGER(args[0]);
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || !gSdlTextures[textureID]) {
        fprintf(stderr, "Runtime error: RenderCopyRect called with invalid TextureID.\n");
        return makeVoid();
    }
    SDL_Rect dstRect = { (int)AS_INTEGER(args[1]), (int)AS_INTEGER(args[2]), (int)AS_INTEGER(args[3]), (int)AS_INTEGER(args[4]) };
    SDL_RenderCopy(gSdlRenderer, gSdlTextures[textureID], NULL, &dstRect);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinSetalphablend) {
    if (arg_count != 1 || args[0].type != TYPE_BOOLEAN) { runtimeError(vm, "SetAlphaBlend expects 1 boolean argument."); return makeVoid(); }
    SDL_BlendMode mode = AS_BOOLEAN(args[0]) ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE;
    if (gSdlRenderer) SDL_SetRenderDrawBlendMode(gSdlRenderer, mode);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinRendertexttotexture) {
    if (arg_count != 4) { runtimeError(vm, "RenderTextToTexture expects 4 arguments."); return makeInt(-1); }
    if (!gSdlFont) { runtimeError(vm, "Font not initialized for RenderTextToTexture."); return makeInt(-1); }

    const char* text = AS_STRING(args[0]);
    SDL_Color color = { (Uint8)AS_INTEGER(args[1]), (Uint8)AS_INTEGER(args[2]), (Uint8)AS_INTEGER(args[3]), 255 };
    
    SDL_Surface* surf = TTF_RenderUTF8_Solid(gSdlFont, text, color);
    if (!surf) return makeInt(-1);

    int surfaceWidth = surf->w;
    int surfaceHeight = surf->h;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(gSdlRenderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) return makeInt(-1);

    int free_slot = -1;
    for(int i = 0; i < MAX_SDL_TEXTURES; i++) { if (!gSdlTextures[i]) { free_slot = i; break; } }
    if (free_slot == -1) { SDL_DestroyTexture(tex); return makeInt(-1); }

    gSdlTextures[free_slot] = tex;
    gSdlTextureWidths[free_slot] = surfaceWidth;
    gSdlTextureHeights[free_slot] = surfaceHeight;
    gSdlTextureAccesses[free_slot] = SDL_TEXTUREACCESS_STATIC;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    return makeInt(free_slot);
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinInittextsystem) {
    if (arg_count != 2) {
        runtimeError(vm, "InitTextSystem expects 2 arguments (FontFileName: String; FontSize: Integer).");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        runtimeError(vm, "Graphics system not initialized before InitTextSystem.");
        return makeVoid();
    }
    // Lazy Initialize SDL_ttf if not already done
    if (!gSdlTtfInitialized) {
        if (TTF_Init() == -1) {
            runtimeError(vm, "SDL_ttf system initialization failed: %s", TTF_GetError());
            return makeVoid();
        }
        gSdlTtfInitialized = true;
    }

    // Get arguments from the args array
    Value fontNameVal = args[0];
    Value fontSizeVal = args[1];

    if (fontNameVal.type != TYPE_STRING || !isIntlikeType(fontSizeVal.type)) {
        runtimeError(vm, "InitTextSystem argument type mismatch. Expected (String, Integer).");
        return makeVoid(); // Don't free args, they are on the VM stack
    }

    const char* font_path = fontNameVal.s_val;
    int font_size = (int)AS_INTEGER(fontSizeVal);

    // Close previous font if one was loaded
    if (gSdlFont) {
        TTF_CloseFont(gSdlFont);
        gSdlFont = NULL;
    }

    gSdlFont = TTF_OpenFont(font_path, font_size);
    if (!gSdlFont) {
        runtimeError(vm, "Failed to load font '%s': %s", font_path, TTF_GetError());
        return makeVoid();
    }
    gSdlFontSize = font_size;

    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinCreatetargettexture) {
    if (arg_count != 2) { runtimeError(vm, "CreateTargetTexture expects 2 arguments (Width, Height: Integer)."); return makeInt(-1); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics system not initialized before CreateTargetTexture."); return makeInt(-1); }

    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) { runtimeError(vm, "CreateTargetTexture arguments must be integers."); return makeInt(-1); }

    int width = (int)AS_INTEGER(args[0]);
    int height = (int)AS_INTEGER(args[1]);

    if (width <= 0 || height <= 0) { runtimeError(vm, "CreateTargetTexture dimensions must be positive."); return makeInt(-1); }

    int textureID = findFreeTextureID();
    if (textureID == -1) { runtimeError(vm, "Maximum number of textures reached."); return makeInt(-1); }

    SDL_Texture* newTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, width, height);
    if (!newTexture) { runtimeError(vm, "SDL_CreateTexture (for target) failed: %s", SDL_GetError()); return makeInt(-1); }

    SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);

    gSdlTextures[textureID] = newTexture;
    gSdlTextureWidths[textureID] = width;
    gSdlTextureHeights[textureID] = height;
    gSdlTextureAccesses[textureID] = SDL_TEXTUREACCESS_TARGET;
    return makeInt(textureID);
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinCreatetexture) {
    if (arg_count != 2) { runtimeError(vm, "CreateTexture expects 2 arguments (Width, Height: Integer)."); return makeInt(-1); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics not initialized before CreateTexture."); return makeInt(-1); }

    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) { runtimeError(vm, "CreateTexture arguments must be integers."); return makeInt(-1); }

    int width = (int)AS_INTEGER(args[0]);
    int height = (int)AS_INTEGER(args[1]);

    if (width <= 0 || height <= 0) { runtimeError(vm, "CreateTexture dimensions must be positive."); return makeInt(-1); }

    int textureID = findFreeTextureID();
    if (textureID == -1) { runtimeError(vm, "Maximum number of textures reached."); return makeInt(-1); }

    SDL_Texture* newTexture = SDL_CreateTexture(gSdlRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!newTexture) { runtimeError(vm, "SDL_CreateTexture failed: %s", SDL_GetError()); return makeInt(-1); }

    SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);

    gSdlTextures[textureID] = newTexture;
    gSdlTextureWidths[textureID] = width;
    gSdlTextureHeights[textureID] = height;
    gSdlTextureAccesses[textureID] = SDL_TEXTUREACCESS_STREAMING;
    return makeInt(textureID);
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinDrawcircle) {
    if (arg_count != 3) { runtimeError(vm, "DrawCircle expects 3 integer arguments (CenterX, CenterY, Radius)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics mode not initialized before DrawCircle."); return makeVoid(); }

    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) || !IS_INTLIKE(args[2])) { runtimeError(vm, "DrawCircle arguments must be integers."); return makeVoid(); }

    int centerX = (int)AS_INTEGER(args[0]);
    int centerY = (int)AS_INTEGER(args[1]);
    int radius = (int)AS_INTEGER(args[2]);

    if (radius < 0) return makeVoid();

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);

    int x = radius;
    int y = 0;
    int err = 0;

    while (x >= y) {
        SDL_RenderDrawPoint(gSdlRenderer, centerX + x, centerY + y);
        SDL_RenderDrawPoint(gSdlRenderer, centerX - x, centerY + y);
        SDL_RenderDrawPoint(gSdlRenderer, centerX + x, centerY - y);
        SDL_RenderDrawPoint(gSdlRenderer, centerX - x, centerY - y);
        SDL_RenderDrawPoint(gSdlRenderer, centerX + y, centerY + x);
        SDL_RenderDrawPoint(gSdlRenderer, centerX - y, centerY + x);
        SDL_RenderDrawPoint(gSdlRenderer, centerX + y, centerY - x);
        SDL_RenderDrawPoint(gSdlRenderer, centerX - y, centerY - x);
        if (err <= 0) {
            y += 1;
            err += 2*y + 1;
        }
        if (err > 0) {
            x -= 1;
            err -= 2*x + 1;
        }
    }
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinDrawline) {
    if (arg_count != 4) { runtimeError(vm, "DrawLine expects 4 integer arguments (x1, y1, x2, y2)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics mode not initialized before DrawLine."); return makeVoid(); }

    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) ||
        !IS_INTLIKE(args[2]) || !IS_INTLIKE(args[3])) { runtimeError(vm, "DrawLine arguments must be integers."); return makeVoid(); }

    int x1 = (int)AS_INTEGER(args[0]);
    int y1 = (int)AS_INTEGER(args[1]);
    int x2 = (int)AS_INTEGER(args[2]);
    int y2 = (int)AS_INTEGER(args[3]);

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    SDL_RenderDrawLine(gSdlRenderer, x1, y1, x2, y2);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinDrawpolygon) {
    if (arg_count != 2) { runtimeError(vm, "DrawPolygon expects 2 arguments (PointsArray, NumPoints)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics not initialized for DrawPolygon."); return makeVoid(); }

    if (args[0].type != TYPE_ARRAY || !IS_INTLIKE(args[1])) { runtimeError(vm, "DrawPolygon argument type mismatch."); return makeVoid(); }
    if (args[0].element_type != TYPE_RECORD) { runtimeError(vm, "DrawPolygon Points argument must be an ARRAY OF PointRecord."); return makeVoid(); }

    int numPoints = (int)AS_INTEGER(args[1]);
    if (numPoints < 2) return makeVoid();

    int total_elements_in_pascal_array = 1;
    for(int i=0; i < args[0].dimensions; ++i) { total_elements_in_pascal_array *= (args[0].upper_bounds[i] - args[0].lower_bounds[i] + 1); }
    if (numPoints > total_elements_in_pascal_array) { runtimeError(vm, "NumPoints exceeds actual size of Pscal PointsArray."); return makeVoid(); }

    SDL_Point* sdlPoints = malloc(sizeof(SDL_Point) * (numPoints + 1));
    if (!sdlPoints) { runtimeError(vm, "Memory allocation failed for SDL_Point array in DrawPolygon."); return makeVoid(); }

    for (int i = 0; i < numPoints; i++) {
        Value recordValue = args[0].array_val[i];
        if (recordValue.type != TYPE_RECORD || !recordValue.record_val) { runtimeError(vm, "Element in PointsArray is not a valid PointRecord."); free(sdlPoints); return makeVoid(); }
        FieldValue* fieldX = recordValue.record_val;
        FieldValue* fieldY = fieldX ? fieldX->next : NULL;

        if (fieldX && strcasecmp(fieldX->name, "x") == 0 && isIntlikeType(fieldX->value.type) &&
            fieldY && strcasecmp(fieldY->name, "y") == 0 && isIntlikeType(fieldY->value.type)) {
            sdlPoints[i].x = (int)AS_INTEGER(fieldX->value);
            sdlPoints[i].y = (int)AS_INTEGER(fieldY->value);
        } else { runtimeError(vm, "PointRecord does not have correct X,Y integer fields."); free(sdlPoints); return makeVoid(); }
    }

    if (numPoints > 1) { sdlPoints[numPoints] = sdlPoints[0]; } // Close the polygon

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    SDL_RenderDrawLines(gSdlRenderer, sdlPoints, numPoints > 1 ? numPoints + 1 : numPoints);

    free(sdlPoints);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinDrawrect) {
    if (arg_count != 4) { runtimeError(vm, "DrawRect expects 4 integer arguments (X1, Y1, X2, Y2)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics mode not initialized before DrawRect."); return makeVoid(); }

    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) ||
        !IS_INTLIKE(args[2]) || !IS_INTLIKE(args[3])) { runtimeError(vm, "DrawRect arguments must be integers."); return makeVoid(); }

    int x1 = (int)AS_INTEGER(args[0]);
    int y1 = (int)AS_INTEGER(args[1]);
    int x2 = (int)AS_INTEGER(args[2]);
    int y2 = (int)AS_INTEGER(args[3]);

    SDL_Rect rect;
    rect.x = (x1 < x2) ? x1 : x2;
    rect.y = (y1 < y2) ? y1 : y2;
    rect.w = abs(x2 - x1) + 1;
    rect.h = abs(y2 - y1) + 1;

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    SDL_RenderDrawRect(gSdlRenderer, &rect);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGetpixelcolor) {
    if (arg_count != 6) { runtimeError(vm, "GetPixelColor expects 6 arguments (X, Y: Integer; var R, G, B, A: Byte)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics not initialized for GetPixelColor."); return makeVoid(); }

    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) { runtimeError(vm, "GetPixelColor X,Y coordinates must be integers."); return makeVoid(); }

    if (args[2].type != TYPE_POINTER || args[3].type != TYPE_POINTER ||
        args[4].type != TYPE_POINTER || args[5].type != TYPE_POINTER) { runtimeError(vm, "GetPixelColor R,G,B,A parameters must be VAR Byte."); return makeVoid(); }

    int x = (int)AS_INTEGER(args[0]);
    int y = (int)AS_INTEGER(args[1]);

    Value* r_ptr = (Value*)args[2].ptr_val;
    Value* g_ptr = (Value*)args[3].ptr_val;
    Value* b_ptr = (Value*)args[4].ptr_val;
    Value* a_ptr = (Value*)args[5].ptr_val;

    if (!r_ptr || !g_ptr || !b_ptr || !a_ptr) { runtimeError(vm, "Null pointer for RGBA output in GetPixelColor."); return makeVoid(); }

    SDL_Rect pixelRect = {x, y, 1, 1};
    Uint8 rgba[4] = {0, 0, 0, 0};

#if defined(PSCALI_SDL3)
    SDL_Surface* surface = SDL_RenderReadPixels(gSdlRenderer, &pixelRect);
    if (!surface) {
        runtimeError(vm, "SDL_RenderReadPixels failed in GetPixelColor: %s", SDL_GetError());
        return makeVoid();
    }
#else
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, 1, 1, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        runtimeError(vm, "Could not create surface for GetPixelColor: %s", SDL_GetError());
        return makeVoid();
    }

    if (SDL_RenderReadPixels(gSdlRenderer, &pixelRect, surface->format->format, surface->pixels, surface->pitch) != 0) {
        runtimeError(vm, "SDL_RenderReadPixels failed in GetPixelColor: %s", SDL_GetError());
        SDL_FreeSurface(surface);
        return makeVoid();
    }
#endif

    Uint32 pixelValue = ((Uint32*)surface->pixels)[0];
    SDL_GetRGBA(pixelValue, surface->format, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);

    SDL_FreeSurface(surface);

    freeValue(r_ptr); *r_ptr = makeByte(rgba[0]);
    freeValue(g_ptr); *g_ptr = makeByte(rgba[1]);
    freeValue(b_ptr); *b_ptr = makeByte(rgba[2]);
    freeValue(a_ptr); *a_ptr = makeByte(rgba[3]);

    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinLoadimagetotexture) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) { runtimeError(vm, "LoadImageToTexture expects 1 argument (FilePath: String)."); return makeInt(-1); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics system not initialized before LoadImageToTexture."); return makeInt(-1); }

    if (!gSdlImageInitialized) {
        int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
        if (!(IMG_Init(imgFlags) & imgFlags)) { runtimeError(vm, "SDL_image initialization failed: %s", IMG_GetError()); return makeInt(-1); }
        gSdlImageInitialized = true;
    }

    const char* filePath = args[0].s_val;
    int free_slot = findFreeTextureID();
    if (free_slot == -1) { runtimeError(vm, "No free texture slots available for LoadImageToTexture."); return makeInt(-1); }

    SDL_Surface* surface = IMG_Load(filePath);
    if (!surface) {
        runtimeError(vm, "Failed to load image '%s': %s", filePath, IMG_GetError());
        return makeInt(-1);
    }

    SDL_Texture* newTexture = SDL_CreateTextureFromSurface(gSdlRenderer, surface);
    if (!newTexture) {
        runtimeError(vm, "Failed to create texture from '%s': %s", filePath, SDL_GetError());
        SDL_FreeSurface(surface);
        return makeInt(-1);
    }

    gSdlTextures[free_slot] = newTexture;
    gSdlTextureWidths[free_slot] = surface->w;
    gSdlTextureHeights[free_slot] = surface->h;
    gSdlTextureAccesses[free_slot] = SDL_TEXTUREACCESS_STATIC;
    SDL_SetTextureBlendMode(newTexture, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surface);
    return makeInt(free_slot);
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinOuttextxy) {
    if (arg_count != 3) { runtimeError(vm, "OutTextXY expects 3 arguments (X, Y: Integer; Text: String)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics system not initialized before OutTextXY."); return makeVoid(); }
    if (!gSdlTtfInitialized || !gSdlFont) { runtimeError(vm, "Text system or font not initialized before OutTextXY."); return makeVoid(); }

    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) || args[2].type != TYPE_STRING) { runtimeError(vm, "OutTextXY argument type mismatch."); return makeVoid(); }

    int x = (int)AS_INTEGER(args[0]);
    int y = (int)AS_INTEGER(args[1]);
    const char* text_to_render = args[2].s_val ? args[2].s_val : "";

    SDL_Surface* textSurface = TTF_RenderUTF8_Solid(gSdlFont, text_to_render, gSdlCurrentColor);
    if (!textSurface) { runtimeError(vm, "TTF_RenderUTF8_Solid failed in OutTextXY: %s", TTF_GetError()); return makeVoid(); }

    SDL_Texture* textTexture = SDL_CreateTextureFromSurface(gSdlRenderer, textSurface);
    if (!textTexture) { SDL_FreeSurface(textSurface); runtimeError(vm, "SDL_CreateTextureFromSurface failed in OutTextXY: %s", SDL_GetError()); return makeVoid(); }

    SDL_Rect destRect = { x, y, textSurface->w, textSurface->h };
    SDL_RenderCopy(gSdlRenderer, textTexture, NULL, &destRect);

    SDL_DestroyTexture(textTexture);
    SDL_FreeSurface(textSurface);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinRendercopy) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        fprintf(stderr, "Runtime error: RenderCopy expects 1 argument (TextureID: Integer).\n");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics not initialized before RenderCopy.\n");
        return makeVoid();
    }

    int textureID = (int)AS_INTEGER(args[0]);
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        fprintf(stderr, "Runtime error: RenderCopy called with invalid TextureID.\n");
        return makeVoid();
    }

    SDL_RenderCopy(gSdlRenderer, gSdlTextures[textureID], NULL, NULL);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinRendercopyex) {
    if (arg_count != 13) {
        fprintf(stderr, "Runtime error: RenderCopyEx expects 13 arguments.\n");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        fprintf(stderr, "Runtime error: Graphics mode not initialized before RenderCopyEx.\n");
        return makeVoid();
    }

    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) || !IS_INTLIKE(args[2]) ||
        !IS_INTLIKE(args[3]) || !IS_INTLIKE(args[4]) || !IS_INTLIKE(args[5]) ||
        !IS_INTLIKE(args[6]) || !IS_INTLIKE(args[7]) || !IS_INTLIKE(args[8]) ||
        !isRealType(args[9].type) || !IS_INTLIKE(args[10]) || !IS_INTLIKE(args[11]) ||
        !IS_INTLIKE(args[12])) {
        fprintf(stderr, "Runtime error: RenderCopyEx argument type mismatch. Expected (Int,Int,Int,Int,Int,Int,Int,Int,Int,Real,Int,Int,Int).\n");
        return makeVoid();
    }

    int textureID = (int)AS_INTEGER(args[0]);
    if (textureID < 0 || textureID >= MAX_SDL_TEXTURES || gSdlTextures[textureID] == NULL) {
        fprintf(stderr, "Runtime error: RenderCopyEx called with invalid or unloaded TextureID.\n");
        return makeVoid();
    }
    SDL_Texture* texture = gSdlTextures[textureID];

    SDL_Rect srcRect = { (int)AS_INTEGER(args[1]), (int)AS_INTEGER(args[2]), (int)AS_INTEGER(args[3]), (int)AS_INTEGER(args[4]) };
    SDL_Rect* srcRectPtr = (srcRect.w > 0 && srcRect.h > 0) ? &srcRect : NULL;

    SDL_Rect dstRect = { (int)AS_INTEGER(args[5]), (int)AS_INTEGER(args[6]), (int)AS_INTEGER(args[7]), (int)AS_INTEGER(args[8]) };

    double angle_degrees = (double)AS_REAL(args[9]);

    SDL_Point rotationCenter;
    SDL_Point* centerPtr = NULL;
    int pscalRotX = (int)AS_INTEGER(args[10]);
    int pscalRotY = (int)AS_INTEGER(args[11]);

    if (pscalRotX >= 0 && pscalRotY >= 0) {
        rotationCenter.x = pscalRotX;
        rotationCenter.y = pscalRotY;
        centerPtr = &rotationCenter;
    }

    SDL_RendererFlip sdl_flip = SDL_FLIP_NONE;
    int flipMode = (int)AS_INTEGER(args[12]);
    if (flipMode == 1) sdl_flip = SDL_FLIP_HORIZONTAL;
    else if (flipMode == 2) sdl_flip = SDL_FLIP_VERTICAL;
    else if (flipMode == 3) sdl_flip = (SDL_RendererFlip)(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL);

    SDL_RenderCopyEx(gSdlRenderer, texture, srcRectPtr, &dstRect, angle_degrees, centerPtr, sdl_flip);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinSetcolor) {
    if (arg_count != 1 || (!IS_INTLIKE(args[0]) && args[0].type != TYPE_BYTE)) {
        runtimeError(vm, "SetColor expects 1 argument (color index 0-255).");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics mode not initialized before SetColor."); return makeVoid(); }

    long long colorCode = AS_INTEGER(args[0]);

    if (colorCode >= 0 && colorCode <= 15) {
         unsigned char intensity = (colorCode > 7) ? 255 : 192;
         gSdlCurrentColor.r = (colorCode & 4) ? intensity : 0;
         gSdlCurrentColor.g = (colorCode & 2) ? intensity : 0;
         gSdlCurrentColor.b = (colorCode & 1) ? intensity : 0;
         if (colorCode == 6) { gSdlCurrentColor.g = intensity / 2; }
         if (colorCode == 7 || colorCode == 15) { gSdlCurrentColor.r=intensity; gSdlCurrentColor.g=intensity; gSdlCurrentColor.b=intensity; }
         if (colorCode == 8) {gSdlCurrentColor.r = 128; gSdlCurrentColor.g = 128; gSdlCurrentColor.b = 128;}
         if (colorCode == 0) {gSdlCurrentColor.r = 0; gSdlCurrentColor.g = 0; gSdlCurrentColor.b = 0;}
    } else {
         int c = (int)(colorCode % 256);
         gSdlCurrentColor.r = (c * 3) % 256;
         gSdlCurrentColor.g = (c * 5) % 256;
         gSdlCurrentColor.b = (c * 7) % 256;
    }
    gSdlCurrentColor.a = 255;

    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);

    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinSetrendertarget) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) { runtimeError(vm, "SetRenderTarget expects 1 argument (TextureID: Integer)."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "Graphics system not initialized before SetRenderTarget."); return makeVoid(); }

    int textureID = (int)AS_INTEGER(args[0]);

    SDL_Texture* targetTexture = NULL;
    if (textureID >= 0 && textureID < MAX_SDL_TEXTURES && gSdlTextures[textureID] != NULL) {
        if (gSdlTextureAccesses[textureID] == SDL_TEXTUREACCESS_TARGET) {
            targetTexture = gSdlTextures[textureID];
        } else {
            runtimeError(vm, "TextureID %d was not created with Target access. Cannot set as render target.", textureID);
        }
    } else if (textureID >= MAX_SDL_TEXTURES || (textureID >=0 && gSdlTextures[textureID] == NULL)) {
        runtimeError(vm, "Invalid TextureID %d passed to SetRenderTarget. Defaulting to screen.", textureID);
    }

    SDL_SetRenderTarget(gSdlRenderer, targetTexture);
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinIskeydown) {
    if (arg_count != 1) {
        runtimeError(vm, "IsKeyDown expects exactly 1 argument (string name or key code).");
        return makeBoolean(false);
    }
    if (!gSdlInitialized || !gSdlWindow) {
        runtimeError(vm, "Graphics mode not initialized before IsKeyDown.");
        return makeBoolean(false);
    }

    bool ok = false;
    SDL_Scancode sc = resolveScancodeFromValue(args[0], &ok);
    if (!ok || sc == SDL_SCANCODE_UNKNOWN || sc < 0 || sc >= SDL_NUM_SCANCODES) {
        runtimeError(vm, "IsKeyDown argument did not resolve to a valid SDL scancode.");
        return makeBoolean(false);
    }

    SDL_PumpEvents();
    const Uint8* state = SDL_GetKeyboardState(NULL);
    if (!state) {
        runtimeError(vm, "SDL_GetKeyboardState returned NULL.");
        return makeBoolean(false);
    }

    return makeBoolean(state[sc] != 0);
}

bool sdlPollNextKey(SDL_Keycode* outCode) {
    if (!outCode || !sdlIsGraphicsActive()) {
        return false;
    }

    SDL_Keycode queuedCode;
    if (dequeuePendingKeycode(&queuedCode)) {
        if (queuedCode == SDLK_q) {
            SDL_DEBUG_SET_BREAK_REQUESTED(1, "PollNextKey queued SDLK_q");
        }
        *outCode = queuedCode;
        return true;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        sdlLogEvent("PollEvent", &event);
#if PSCALI_HAS_SYSWM
#if PSCALI_HAS_SYSWM
#if PSCALI_HAS_SYSWM
        if (event.type == SDL_SYSWMEVENT) {
            handleSysWmEvent(&event);
            continue;
        }
#endif
#endif
#endif

        if (event.type == SDL_QUIT) {
            SDL_DEBUG_SET_BREAK_REQUESTED(1, "PollNextKey SDL_QUIT");
            return false;
        }

        if (isWindowCloseEvent(&event)) {
            continue;
        }

        if (event.type == SDL_KEYDOWN) {
            SDL_Keycode sym = event.key.keysym.sym;
            if (sym == SDLK_q) {
                SDL_DEBUG_SET_BREAK_REQUESTED(1, "PollNextKey SDL_KEYDOWN q");
            }

            bool textActive = sdlTextInputActive();
            if (!textActive || !isPrintableKeycode(sym)) {
                enqueuePendingKeycode(sym);
            }

            if (dequeuePendingKeycode(&queuedCode)) {
                *outCode = queuedCode;
                return true;
            }
        } else if (event.type == SDL_TEXTINPUT) {
            enqueueUtf8Text(event.text.text);
            if (dequeuePendingKeycode(&queuedCode)) {
                *outCode = queuedCode;
                return true;
            }
        }
    }

    return false;
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinPollkey) {
    if (arg_count != 0) {
        runtimeError(vm, "PollKey expects 0 arguments.");
        return makeInt(0);
    }
    if (!gSdlInitialized || !gSdlWindow) {
        runtimeError(vm, "Graphics mode not initialized before PollKey.");
        return makeInt(0);
    }

    SDL_Keycode code;
    if (sdlPollNextKey(&code)) {
        SDL_DEBUG_LOG("PollKey returning code=%d\n", (int)code);
        return makeInt((int)code);
    }
    return makeInt(0);
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinWaitkeyevent) {
    if (arg_count != 0) { runtimeError(vm, "WaitKeyEvent expects 0 arguments."); return makeVoid(); }
    if (!gSdlInitialized || !gSdlWindow) { runtimeError(vm, "Graphics mode not initialized before WaitKeyEvent."); return makeVoid(); }

    if (hasPendingKeycode()) {
        return makeVoid();
    }

    SDL_Event event;
    int waiting = 1;
    while (waiting) {
        if (SDL_WaitEvent(&event)) {
            sdlLogEvent("WaitKeyEvent", &event);
#if PSCALI_HAS_SYSWM
            if (event.type == SDL_SYSWMEVENT) {
                handleSysWmEvent(&event);
                continue;
            }
#endif

#if defined(PSCAL_TARGET_IOS)
            if (event.type == SDL_QUIT) {
                continue;
            } else if (isWindowCloseEvent(&event)) {
                continue;
            } else if (event.type == SDL_KEYDOWN) {
                SDL_Keycode sym = event.key.keysym.sym;
                if (sym == SDLK_q) {
                    SDL_DEBUG_SET_BREAK_REQUESTED(1, "WaitKey(iOS) SDL_KEYDOWN q");
                }

                bool textActive = sdlTextInputActive();
                if (!textActive || !isPrintableKeycode(sym)) {
                    enqueuePendingKeycode(sym);
                    waiting = 0;
                }
            } else if (event.type == SDL_TEXTINPUT) {
                enqueueUtf8Text(event.text.text);
                waiting = 0;
            }
#else
        if (event.type == SDL_QUIT) {
            SDL_DEBUG_SET_BREAK_REQUESTED(1, "WaitKey SDL_QUIT");
            waiting = 0;
        } else if (isWindowCloseEvent(&event)) {
            continue;
        } else if (event.type == SDL_KEYDOWN) {
            SDL_Keycode sym = event.key.keysym.sym;
            if (sym == SDLK_q) {
                SDL_DEBUG_SET_BREAK_REQUESTED(1, "WaitKey SDL_KEYDOWN q");
            }

            bool textActive = sdlTextInputActive();
            if (!textActive || !isPrintableKeycode(sym)) {
                enqueuePendingKeycode(sym);
                waiting = 0;
            }
            } else if (event.type == SDL_TEXTINPUT) {
                enqueueUtf8Text(event.text.text);
                waiting = 0;
            }
#endif
        } else {
            runtimeError(vm, "SDL_WaitEvent failed: %s", SDL_GetError());
            waiting = 0;
        }
    }
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinFillcircle) {
    if (arg_count != 3) {
        runtimeError(vm, "FillCircle expects 3 integer arguments (CenterX, CenterY, Radius).");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        runtimeError(vm, "Graphics mode not initialized before FillCircle.");
        return makeVoid();
    }

    // Accept either integer or real arguments and coerce to integers.
    // Some front ends (e.g. the clike compiler) may supply real values
    // when calling fillcircle.  Previously this triggered a runtime
    // error and nothing was drawn.  To make the builtin more forgiving
    // we transparently truncate real arguments to integers.
    int centerX, centerY, radius;
    if (IS_INTLIKE(args[0])) {
        centerX = (int)AS_INTEGER(args[0]);
    } else if (isRealType(args[0].type)) {
        centerX = (int)AS_REAL(args[0]);
    } else {
        runtimeError(vm, "FillCircle argument 1 must be numeric.");
        return makeVoid();
    }

    if (IS_INTLIKE(args[1])) {
        centerY = (int)AS_INTEGER(args[1]);
    } else if (isRealType(args[1].type)) {
        centerY = (int)AS_REAL(args[1]);
    } else {
        runtimeError(vm, "FillCircle argument 2 must be numeric.");
        return makeVoid();
    }

    if (IS_INTLIKE(args[2])) {
        radius = (int)AS_INTEGER(args[2]);
    } else if (isRealType(args[2].type)) {
        radius = (int)AS_REAL(args[2]);
    } else {
        runtimeError(vm, "FillCircle argument 3 must be numeric.");
        return makeVoid();
    }

    if (radius < 0) return makeVoid();

    // Set the draw color from the global state
    SDL_SetRenderDrawColor(
        gSdlRenderer,
        gSdlCurrentColor.r,
        gSdlCurrentColor.g,
        gSdlCurrentColor.b,
        gSdlCurrentColor.a);

    /*
     * Previous implementation used horizontal lines with sqrt/floor to
     * compute the circle span.  On some platforms this produced rounding
     * issues that resulted in nothing being rendered for small radii.  The
     * implementation below simply iterates over a bounding box and plots
     * any point that falls inside the circle (dx*dx + dy*dy <= r^2).  This
     * is a little more brute force but guarantees that every pixel inside
     * the circle is touched, making the balls visible on all systems.
     */
    int r2 = radius * radius;
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= r2) {
                SDL_RenderDrawPoint(gSdlRenderer, centerX + dx, centerY + dy);
            }
        }
    }

    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinGraphloop) {
    if (arg_count != 1) {
        runtimeError(vm, "GraphLoop expects 1 argument (milliseconds).");
        return makeVoid();
    }
    long long ms;
    if (IS_INTLIKE(args[0]) || args[0].type == TYPE_WORD || args[0].type == TYPE_BYTE) {
        ms = AS_INTEGER(args[0]);
    } else if (isRealType(args[0].type)) {
        double delay = AS_REAL(args[0]);
        if (!isfinite(delay)) {
            runtimeError(vm, "GraphLoop delay must be finite.");
            return makeVoid();
        }
        if (delay > (double)LLONG_MAX || delay < (double)LLONG_MIN) {
            runtimeError(vm, "GraphLoop delay is out of range.");
            return makeVoid();
        }
        ms = (long long)delay;
    } else {
        runtimeError(vm, "GraphLoop argument must be an integer-like type.");
        return makeVoid();
    }

    if (ms < 0) ms = 0;

    if (gSdlInitialized && gSdlWindow && gSdlRenderer) {
        Uint64 startTime = PSCAL_SDL_GET_TICKS();
        Uint64 targetTime = startTime + (Uint64)ms;
        SDL_Event event;

        /*
         * Always process at least one batch of window events.  Some window
         * managers (e.g. GNOME on Linux) show an "application is not
         * responding" dialog if we miss several compositor pings, which
         * happens whenever the frame takes longer than the requested delay.
         */
        for (;;) {
            SDL_PumpEvents();

            while (SDL_PollEvent(&event)) {
#if PSCALI_HAS_SYSWM
                if (event.type == SDL_SYSWMEVENT) {
                    handleSysWmEvent(&event);
                    continue;
                }
#endif

                if (event.type == SDL_QUIT) {
                    SDL_DEBUG_SET_BREAK_REQUESTED(1, "GraphLoop SDL_QUIT");
                    return makeVoid();
                }

                if (isWindowCloseEvent(&event)) {
                    /* Swallow window close requests; _NET_WM_PING is handled separately in handleSysWmEvent. */
                    continue;
                }

                if (event.type == SDL_KEYDOWN) {
                    SDL_Keycode sym = event.key.keysym.sym;
                    if (sym == SDLK_q) {
                        SDL_DEBUG_SET_BREAK_REQUESTED(1, "GraphLoop SDL_KEYDOWN q");
                    }

                    bool textActive = sdlTextInputActive();
                    if (!textActive || !isPrintableKeycode(sym)) {
                        enqueuePendingKeycode(sym);
                    }
                } else if (event.type == SDL_TEXTINPUT) {
                    enqueueUtf8Text(event.text.text);
                }
            }

            if (break_requested) {
                return makeVoid();
            }

            Uint64 now = PSCAL_SDL_GET_TICKS();
            if (now >= targetTime) {
                break;
            }

            Uint64 remaining = targetTime - now;
            if (remaining > 10) {
                remaining = 10;
            }

            if (remaining > 0) {
                SDL_Delay((Uint32)remaining);
            }
        }
    }
    return makeVoid();
}

PSCAL_DEFINE_IOS_SDL_BUILTIN(vmBuiltinPutpixel) {
    if (arg_count != 2) {
        runtimeError(vm, "PutPixel expects 2 arguments (X, Y).");
        return makeVoid();
    }
    if (!gSdlInitialized || !gSdlRenderer) {
        runtimeError(vm, "Graphics mode not initialized before PutPixel.");
        return makeVoid();
    }

    if (!IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "PutPixel coordinates must be integers.");
        return makeVoid();
    }

    int x = (int)AS_INTEGER(args[0]);
    int y = (int)AS_INTEGER(args[1]);

    // Set the draw color from the global state
    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
    
    // Draw the point
    SDL_RenderDrawPoint(gSdlRenderer, x, y);
    
    return makeVoid();
}

bool sdlIsGraphicsActive(void) {
    if (!gSdlInitialized || gSdlWindow == NULL) {
        return false;
    }

    if (gSdlRenderer != NULL) {
        return true;
    }

    if (gSdlGLContext != NULL) {
        return true;
    }

    return false;
}

static void pumpKeyEvents(void) {
    if (!sdlIsGraphicsActive()) {
        return;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_SYSWMEVENT) {
            handleSysWmEvent(&event);
            continue;
        }

        if (event.type == SDL_QUIT) {
            SDL_DEBUG_SET_BREAK_REQUESTED(1, "PumpKeyEvents SDL_QUIT");
            continue;
        }

        if (isWindowCloseEvent(&event)) {
            continue;
        }

        if (event.type == SDL_KEYDOWN) {
            SDL_Keycode sym = event.key.keysym.sym;
            if (sym == SDLK_q) {
                SDL_DEBUG_SET_BREAK_REQUESTED(1, "PumpKeyEvents SDL_KEYDOWN q");
            }

            bool textActive = sdlTextInputActive();
            if (!textActive || !isPrintableKeycode(sym)) {
                enqueuePendingKeycode(sym);
            }
        } else if (event.type == SDL_TEXTINPUT) {
            enqueueUtf8Text(event.text.text);
        }
    }
}

bool sdlHasPendingKeycode(void) {
    if (!sdlIsGraphicsActive()) {
        return false;
    }

    if (hasPendingKeycode()) {
        return true;
    }

    pumpKeyEvents();
    return hasPendingKeycode();
}

SDL_Keycode sdlWaitNextKeycode(void) {
    if (!sdlIsGraphicsActive()) {
        return SDLK_UNKNOWN;
    }

    SDL_Keycode code;
    if (dequeuePendingKeycode(&code)) {
        return code;
    }

    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        if (event.type == SDL_SYSWMEVENT) {
            handleSysWmEvent(&event);
            continue;
        }

        sdlLogEvent("WaitNextKey", &event);
        if (event.type == SDL_QUIT) {
            SDL_DEBUG_SET_BREAK_REQUESTED(1, "WaitNextKey SDL_QUIT");
            return SDLK_UNKNOWN;
        }

        if (isWindowCloseEvent(&event)) {
            continue;
        }

        if (event.type == SDL_KEYDOWN) {
            SDL_Keycode sym = event.key.keysym.sym;
            if (sym == SDLK_q) {
                SDL_DEBUG_SET_BREAK_REQUESTED(1, "WaitNextKey SDL_KEYDOWN q");
            }

            bool textActive = sdlTextInputActive();
            if (!textActive || !isPrintableKeycode(sym)) {
                enqueuePendingKeycode(sym);
            }

            if (dequeuePendingKeycode(&code)) {
                return code;
            }
        } else if (event.type == SDL_TEXTINPUT) {
            enqueueUtf8Text(event.text.text);
            if (dequeuePendingKeycode(&code)) {
                return code;
            }
        }
    }

    return SDLK_UNKNOWN;
}
#endif
