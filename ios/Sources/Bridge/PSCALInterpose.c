#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/syscall.h> 
#include <sys/uio.h>
#include <termios.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include "PSCALBuildConfig.h"
#include "ios/vproc.h"

// --- Configuration Macros ---

#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST "__DATA_CONST"
#endif

#ifndef PSCAL_USE_DYLD_INTERPOSE
#define PSCAL_USE_DYLD_INTERPOSE 0
#endif

#define PSCAL_DYLD_INTERPOSE(_replacement, _replacee)                \
    __attribute__((used)) static struct {                            \
        const void *replacement;                                     \
        const void *replacee;                                        \
    } _pscal_interpose_##_replacee                                   \
        __attribute__((section("__DATA,__interpose"))) = {           \
            (const void *)(uintptr_t)&_replacement,                  \
            (const void *)(uintptr_t)&_replacee                      \
        }

// syscall() is deprecated/unsupported on iOS and unreliable on Catalyst.
// Limit raw syscall fallbacks to macOS only.
#if defined(__APPLE__) && defined(TARGET_OS_OSX) && TARGET_OS_OSX && \
    (!defined(TARGET_OS_MACCATALYST) || !TARGET_OS_MACCATALYST)
#define PSCAL_ENABLE_SYSCALL_FALLBACK 1
#else
#define PSCAL_ENABLE_SYSCALL_FALLBACK 0
#endif

#if !PSCAL_USE_DYLD_INTERPOSE
#undef PSCAL_DYLD_INTERPOSE
#define PSCAL_DYLD_INTERPOSE(_replacement, _replacee)
#endif

#if defined(__arm64e__) && defined(__has_include)
#if __has_include(<ptrauth.h>)
#include <ptrauth.h>
#define PSCAL_SIGN_POINTER(ptr, addr) \
    ptrauth_sign_unauthenticated((void *)(ptr), ptrauth_key_asia, ptrauth_blend_discriminator((void *)(addr), 0))
#else
#define PSCAL_SIGN_POINTER(ptr, addr) (ptr)
#endif
#else
#define PSCAL_SIGN_POINTER(ptr, addr) (ptr)
#endif

// --- Global State ---
// Use a global bypass only before bootstrap (TLS may be unavailable), then
// fall back to per-thread depth so one blocking raw call doesn't disable
// interposition across all threads.
static volatile atomic_int gInterposeBypassGlobal = 0;
static _Thread_local int gInterposeBypassDepth = 0;
static _Thread_local int gInterposeGuardDepth = 0;
static atomic_uintptr_t gInterposeResolveOwner = 0;
static atomic_int gInterposeResolveDepth = 0;
static volatile sig_atomic_t gInterposeBootstrapped = 0;
static pthread_t gInterposeMainThread;
static volatile sig_atomic_t gInterposeMainThreadSet = 0;
static volatile sig_atomic_t gInterposeMasterEnabled = 0;
static volatile sig_atomic_t gInterposeFeatureEnabled = 0;

// --- Forward Declarations ---
static const char *pscalInterposeAppBundlePrefix(void);
static int pscal_interpose_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg);
static int pscal_interpose_open(const char *path, int flags, ...);
static int pscal_interpose_fstat(int fd, struct stat *st);
static int pscal_interpose_stat(const char *path, struct stat *st);
static int pscal_interpose_lstat(const char *path, struct stat *st);
static pid_t pscal_interpose_getpid(void);
static void pscalInterposeInstallHooks(void);
// Explicit declaration for the raw wrapper to avoid recursion in isatty
static int pscalRawIoctl(int fd, unsigned long request, ...); 

// Some builds may link a vproc library without this symbol; keep interpose safe.
__attribute__((weak)) int vprocThreadHasActiveVproc(void) {
    return 1;
}

// --- Recursion & Bypass Guards ---
static uintptr_t pscalInterposeThreadId(void) {
    return (uintptr_t)pthread_self();
}

static bool pscalIsResolving(void) {
    return atomic_load(&gInterposeResolveOwner) == pscalInterposeThreadId();
}

static void pscalResolveEnter(void) {
    uintptr_t tid = pscalInterposeThreadId();
    if (atomic_load(&gInterposeResolveOwner) == tid) {
        atomic_fetch_add(&gInterposeResolveDepth, 1);
        return;
    }
    for (;;) {
        uintptr_t expected = 0;
        if (atomic_compare_exchange_weak(&gInterposeResolveOwner, &expected, tid)) {
            atomic_store(&gInterposeResolveDepth, 1);
            return;
        }
        while (atomic_load(&gInterposeResolveOwner) != 0) { }
    }
}

static void pscalResolveLeave(void) {
    uintptr_t tid = pscalInterposeThreadId();
    if (atomic_load(&gInterposeResolveOwner) != tid) return;
    int depth = atomic_fetch_sub(&gInterposeResolveDepth, 1);
    if (depth <= 1) {
        atomic_store(&gInterposeResolveOwner, 0);
    }
}

static void pscalInterposeEnterRaw(void) {
    if (!gInterposeBootstrapped) {
        atomic_fetch_add(&gInterposeBypassGlobal, 1);
        return;
    }
    gInterposeBypassDepth++;
}

static void pscalInterposeExitRaw(void) {
    if (!gInterposeBootstrapped) {
        atomic_fetch_sub(&gInterposeBypassGlobal, 1);
        return;
    }
    if (gInterposeBypassDepth > 0) {
        gInterposeBypassDepth--;
    }
}

static __attribute__((unused)) int pscalInterposeBypassActive(void) {
    if (!gInterposeBootstrapped) {
        return atomic_load(&gInterposeBypassGlobal) > 0;
    }
    return gInterposeBypassDepth > 0;
}

// --- Resolution Helpers ---
static const void *pscalInterposeSelfBase(void) {
    static const void *self_base = NULL;
    static atomic_int self_base_state = ATOMIC_VAR_INIT(0);
    if (atomic_load(&self_base_state) == 2) return self_base;
    int expected = 0;
    if (atomic_compare_exchange_strong(&self_base_state, &expected, 1)) {
        Dl_info info;
        if (dladdr((void *)&pscalInterposeSelfBase, &info) != 0) {
            self_base = info.dli_fbase;
        }
        atomic_store(&self_base_state, 2);
    } else {
        while (atomic_load(&self_base_state) == 1) { }
    }
    return self_base;
}

static void *pscalInterposeResolveGeneric(const char *name, void *handle) {
    if (pscalIsResolving()) return NULL;
    pscalResolveEnter();
    void *sym = dlsym(handle, name);
    pscalResolveLeave();
    
    // Prevent self-binding
    const void *self_base = pscalInterposeSelfBase();
    if (sym && self_base) {
        Dl_info sym_info;
        if (dladdr(sym, &sym_info) != 0 && sym_info.dli_fbase == self_base) {
            return NULL;
        }
    }
    return sym;
}

static void *pscalInterposeResolveDefault(const char *name) {
    return pscalInterposeResolveGeneric(name, RTLD_DEFAULT);
}

static bool pscalInterposePathIsInAppBundle(const char *path) {
    if (!path) return false;
    const char *prefix = pscalInterposeAppBundlePrefix();
    if (!prefix || prefix[0] == '\0') return false;
    size_t prefix_len = strlen(prefix);
    if (strncmp(path, prefix, prefix_len) != 0) return false;
    return path[prefix_len] == '/' || path[prefix_len] == '\0';
}

static __attribute__((unused)) bool pscalInterposeSymbolIsInAppBundle(void *sym) {
    if (!sym) return false;
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(sym, &info) == 0) return false;
    return pscalInterposePathIsInAppBundle(info.dli_fname);
}

static bool pscalInterposePathHasBasename(const char *path, const char *basename) {
    if (!path || !basename) return false;
    size_t path_len = strlen(path);
    size_t base_len = strlen(basename);
    if (path_len < base_len) return false;
    const char *tail = path + (path_len - base_len);
    if (strcmp(tail, basename) != 0) return false;
    if (tail == path) return true;
    return tail[-1] == '/';
}

static bool pscalInterposePathIsSystemLibrary(const char *path) {
    if (!path) return false;
    if (strncmp(path, "/usr/lib/system/", 16) == 0) return true;
    if (strncmp(path, "/System/iOSSupport/usr/lib/system/", 33) == 0) return true;
    if (strcmp(path, "/usr/lib/libSystem.B.dylib") == 0) return true;
    if (strcmp(path, "/usr/lib/libSystem.dylib") == 0) return true;
    if (strcmp(path, "/System/iOSSupport/usr/lib/libSystem.B.dylib") == 0) return true;
    if (strcmp(path, "/System/iOSSupport/usr/lib/libSystem.dylib") == 0) return true;
    if (strcmp(path, "/usr/lib/system/libsystem_c.dylib") == 0) return true;
    if (strcmp(path, "/usr/lib/system/libsystem_kernel.dylib") == 0) return true;
    if (strcmp(path, "/System/iOSSupport/usr/lib/system/libsystem_kernel.dylib") == 0) return true;
    if (pscalInterposePathHasBasename(path, "libsystem_kernel.dylib")) return true;
    if (pscalInterposePathHasBasename(path, "libsystem_c.dylib")) return true;
    if (pscalInterposePathHasBasename(path, "libSystem.B.dylib")) return true;
    if (pscalInterposePathHasBasename(path, "libSystem.dylib")) return true;
    return false;
}

static bool pscalInterposeSymbolIsSystemLibrary(void *sym) {
    if (!sym) return false;
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(sym, &info) == 0) return false;
    return pscalInterposePathIsSystemLibrary(info.dli_fname);
}

static bool pscalInterposePathIsKernelLibrary(const char *path) {
    if (!path) return false;
    if (strcmp(path, "/usr/lib/system/libsystem_kernel.dylib") == 0) return true;
    if (strcmp(path, "/System/iOSSupport/usr/lib/system/libsystem_kernel.dylib") == 0) return true;
    if (pscalInterposePathHasBasename(path, "libsystem_kernel.dylib")) return true;
    return false;
}

static bool pscalInterposeSymbolIsKernelLibrary(void *sym) {
    if (!sym) return false;
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(sym, &info) == 0) return false;
    return pscalInterposePathIsKernelLibrary(info.dli_fname);
}

static bool pscalInterposeSymbolIsLogRedirect(void *sym) {
    if (!sym) return false;
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(sym, &info) == 0) return false;
    if (info.dli_sname) {
        if (strstr(info.dli_sname, "LogRedirect") != NULL) return true;
        if (strstr(info.dli_sname, "logredirect") != NULL) return true;
    }
    if (info.dli_fname) {
        if (strstr(info.dli_fname, "LogRedirect") != NULL) return true;
        if (strstr(info.dli_fname, "logredirect") != NULL) return true;
    }
    return false;
}

static bool pscalInterposeCallerIsLogRedirect(void) {
    void *ra = __builtin_return_address(0);
    return pscalInterposeSymbolIsLogRedirect(ra);
}

static void *pscalInterposeKernelHandle(void) {
#if defined(__APPLE__)
    static void *handle = NULL;
    static atomic_int handle_state = ATOMIC_VAR_INIT(0);
    if (atomic_load(&handle_state) == 2) {
        return handle;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&handle_state, &expected, 1)) {
        void *resolved = NULL;
        if (!pscalIsResolving()) {
            pscalResolveEnter();
            const char *candidates[] = {
                "/usr/lib/system/libsystem_kernel.dylib",
                "/System/iOSSupport/usr/lib/system/libsystem_kernel.dylib",
                "libSystem.B.dylib",
                "libSystem.dylib",
                "/usr/lib/libSystem.B.dylib",
                "/usr/lib/libSystem.dylib",
                "/usr/lib/system/libsystem_c.dylib",
                NULL
            };
            for (size_t i = 0; candidates[i]; ++i) {
#ifdef RTLD_NOLOAD
                resolved = dlopen(candidates[i], RTLD_LAZY | RTLD_NOLOAD);
#endif
                if (!resolved) {
                    resolved = dlopen(candidates[i], RTLD_LAZY);
                }
                if (resolved) {
                    break;
                }
            }
            pscalResolveLeave();
        }
        if (resolved) {
            handle = resolved;
            atomic_store(&handle_state, 2);
        } else {
            atomic_store(&handle_state, 0);
        }
    } else {
        while (atomic_load(&handle_state) == 1) { }
    }
    return handle;
#else
    return NULL;
#endif
}

static void *pscalInterposeResolveKernel(const char *name) {
    void *handle = pscalInterposeKernelHandle();
    if (!handle) return NULL;
    void *sym = pscalInterposeResolveGeneric(name, handle);
    if (pscalInterposeSymbolIsLogRedirect(sym)) return NULL;
    if (!pscalInterposeSymbolIsKernelLibrary(sym)) return NULL;
    return sym;
}

static void *pscalInterposeResolveSystem(const char *name) {
    void *sym = pscalInterposeResolveKernel(name);
    if (sym && pscalInterposeSymbolIsSystemLibrary(sym)) return sym;

    sym = pscalInterposeResolveGeneric(name, RTLD_NEXT);
    if (pscalInterposeSymbolIsLogRedirect(sym)) return NULL;
    if (sym && pscalInterposeSymbolIsSystemLibrary(sym)) return sym;

    sym = pscalInterposeResolveDefault(name);
    if (pscalInterposeSymbolIsLogRedirect(sym)) return NULL;
    if (sym && pscalInterposeSymbolIsSystemLibrary(sym)) return sym;

    return NULL;
}

// --- Runtime Rebinding (fishhook-style) ---

#if defined(__APPLE__)
struct pscalRebinding {
    const char *name;
    void *replacement;
    void **replaced;
};

struct pscalRebindingsEntry {
    struct pscalRebinding *rebindings;
    size_t rebindings_nel;
    struct pscalRebindingsEntry *next;
};

static struct pscalRebindingsEntry *gPscalRebindings = NULL;

static int pscalAppendRebindings(struct pscalRebindingsEntry **head, struct pscalRebinding rebindings[], size_t rebindings_nel) {
    if (!head || !rebindings || rebindings_nel == 0) return -1;
    struct pscalRebindingsEntry *entry = (struct pscalRebindingsEntry *)malloc(sizeof(*entry));
    if (!entry) return -1;
    entry->rebindings = (struct pscalRebinding *)malloc(sizeof(struct pscalRebinding) * rebindings_nel);
    if (!entry->rebindings) {
        free(entry);
        return -1;
    }
    memcpy(entry->rebindings, rebindings, sizeof(struct pscalRebinding) * rebindings_nel);
    entry->rebindings_nel = rebindings_nel;
    entry->next = *head;
    *head = entry;
    return 0;
}

static void pscalPerformRebindingWithSection(struct pscalRebindingsEntry *rebindings,
                                             const struct section_64 *section,
                                             intptr_t slide,
                                             const struct nlist_64 *symtab,
                                             const char *strtab,
                                             const uint32_t *indirect_symtab) {
    uint32_t type = section->flags & SECTION_TYPE;
    if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) return;

    uint32_t *indirect_symbol_indices = (uint32_t *)(indirect_symtab + section->reserved1);
    void **indirect_symbol_bindings = (void **)((uintptr_t)slide + section->addr);
    size_t count = section->size / sizeof(void *);
    vm_address_t address = (vm_address_t)indirect_symbol_bindings;
    vm_size_t size = (vm_size_t)section->size;
    if (size == 0) return;
    (void)vm_protect(mach_task_self(), address, size, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);

    for (size_t i = 0; i < count; ++i) {
        uint32_t symtab_index = indirect_symbol_indices[i];
        if (symtab_index == INDIRECT_SYMBOL_ABS ||
            symtab_index == INDIRECT_SYMBOL_LOCAL ||
            symtab_index == (INDIRECT_SYMBOL_ABS | INDIRECT_SYMBOL_LOCAL)) {
            continue;
        }
        uint32_t strx = symtab[symtab_index].n_un.n_strx;
        if (strx == 0) continue;
        const char *symbol_name = strtab + strx;
        if (!symbol_name) continue;
        if (symbol_name[0] == '_') symbol_name++;
        for (struct pscalRebindingsEntry *entry = rebindings; entry; entry = entry->next) {
            for (size_t j = 0; j < entry->rebindings_nel; ++j) {
                if (strcmp(symbol_name, entry->rebindings[j].name) != 0) continue;
                if (entry->rebindings[j].replaced && *(entry->rebindings[j].replaced) == NULL) {
                    *(entry->rebindings[j].replaced) = indirect_symbol_bindings[i];
                }
                indirect_symbol_bindings[i] = PSCAL_SIGN_POINTER(entry->rebindings[j].replacement, &indirect_symbol_bindings[i]);
                break;
            }
        }
    }

    (void)vm_protect(mach_task_self(), address, size, false, VM_PROT_READ);
}

static void pscalRebindSymbolsForImage(const struct mach_header *header, intptr_t slide) {
    if (!header) return;
    if (header->magic != MH_MAGIC_64 && header->magic != MH_CIGAM_64) return;

    const struct mach_header_64 *mh = (const struct mach_header_64 *)header;
    const struct load_command *cmd = (const struct load_command *)(mh + 1);
    const struct segment_command_64 *linkedit_segment = NULL;
    const struct symtab_command *symtab_cmd = NULL;
    const struct dysymtab_command *dysymtab_cmd = NULL;

    for (uint32_t i = 0; i < mh->ncmds; ++i) {
        if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg_cmd = (const struct segment_command_64 *)cmd;
            if (strcmp(seg_cmd->segname, SEG_LINKEDIT) == 0) {
                linkedit_segment = seg_cmd;
            }
        } else if (cmd->cmd == LC_SYMTAB) {
            symtab_cmd = (const struct symtab_command *)cmd;
        } else if (cmd->cmd == LC_DYSYMTAB) {
            dysymtab_cmd = (const struct dysymtab_command *)cmd;
        }
        cmd = (const struct load_command *)((const uint8_t *)cmd + cmd->cmdsize);
    }

    if (!linkedit_segment || !symtab_cmd || !dysymtab_cmd) return;

    uintptr_t linkedit_base = (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
    const struct nlist_64 *symtab = (const struct nlist_64 *)(linkedit_base + symtab_cmd->symoff);
    const char *strtab = (const char *)(linkedit_base + symtab_cmd->stroff);
    const uint32_t *indirect_symtab = (const uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

    cmd = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; ++i) {
        if (cmd->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg_cmd = (const struct segment_command_64 *)cmd;
            if (strcmp(seg_cmd->segname, SEG_DATA) != 0 && strcmp(seg_cmd->segname, SEG_DATA_CONST) != 0) {
                cmd = (const struct load_command *)((const uint8_t *)cmd + cmd->cmdsize);
                continue;
            }
            const struct section_64 *section = (const struct section_64 *)((const uint8_t *)seg_cmd + sizeof(*seg_cmd));
            for (uint32_t j = 0; j < seg_cmd->nsects; ++j) {
                pscalPerformRebindingWithSection(gPscalRebindings, &section[j], slide, symtab, strtab, indirect_symtab);
            }
        }
        cmd = (const struct load_command *)((const uint8_t *)cmd + cmd->cmdsize);
    }
}

static void pscalRebindCallback(const struct mach_header *header, intptr_t slide) {
    pscalRebindSymbolsForImage(header, slide);
}

static int pscalRebindSymbols(const struct pscalRebinding rebindings[], size_t rebindings_nel) {
    bool first = (gPscalRebindings == NULL);
    if (pscalAppendRebindings(&gPscalRebindings, (struct pscalRebinding *)rebindings, rebindings_nel) < 0) {
        return -1;
    }
    if (first) {
        _dyld_register_func_for_add_image(pscalRebindCallback);
    }
    uint32_t image_count = _dyld_image_count();
    for (uint32_t i = 0; i < image_count; ++i) {
        pscalRebindSymbolsForImage(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
    }
    return 0;
}
#endif

// --- Raw Wrapper Prototypes ---
static int pscalRawStat(const char *path, struct stat *st);
static int pscalRawOpen(const char *path, int flags, int mode, int has_mode);
static int pscalRawFstat(int fd, struct stat *st);
static ssize_t pscalRawRead(int fd, void *buf, size_t count);
static ssize_t pscalRawWrite(int fd, const void *buf, size_t count);
static ssize_t pscalRawWriteKernel(int fd, const void *buf, size_t count);
static ssize_t pscalRawReadv(int fd, const struct iovec *iov, int iovcnt);
static int pscalRawClose(int fd);
static pid_t pscalRawGetpid(void);
static int pscalRawAccess(const char *path, int mode);
static ssize_t pscalRawWritev(int fd, const struct iovec *iov, int iovcnt);

// --- Bootstrap ---

// Pre-resolve critical functions to avoid recursion during AppKit init
static void pscalWarmUpInterposers(void) {
    pscalRawGetpid();
    int null_read = pscalRawOpen("/dev/null", O_RDONLY, 0, 0);
    if (null_read >= 0) {
        char scratch = 0;
        pscalRawRead(null_read, &scratch, 0);
        pscalRawClose(null_read);
    }
    int null_write = pscalRawOpen("/dev/null", O_WRONLY, 0, 0);
    if (null_write >= 0) {
        char scratch = 0;
        pscalRawWrite(null_write, &scratch, 0);
        pscalRawWriteKernel(null_write, &scratch, 0);
        pscalRawClose(null_write);
    }
    pscalRawClose(-1);
    struct stat st;
    pscalRawStat("/dev/null", &st);
    pscalRawAccess("/dev/null", F_OK);
}

void PSCALRuntimeInterposeBootstrap(void) {
    if (gInterposeBootstrapped) return;
    
    // Warm up critical symbols immediately.
    // This populates the 'static (*fn)' variables inside the raw wrappers.
    pscalWarmUpInterposers();
    
    gInterposeMasterEnabled = 0;
#if defined(__APPLE__)
    if (pthread_main_np()) {
        gInterposeMainThread = pthread_self();
        gInterposeMainThreadSet = 1;
        vprocRegisterInterposeBypassThread(gInterposeMainThread);
    }
#endif

    pscalInterposeInstallHooks();
    
    // CRITICAL FIX: Disabled Bundle Preflight.
    // Running CFBundleCreate during library initialization corrupts 
    // the runtime state (PAC signatures) on arm64e/Catalyst.
    // pscalInterposeBundlePreflight(); 
    
    gInterposeFeatureEnabled = 1;
    gInterposeMasterEnabled = 1;
    gInterposeBootstrapped = 1;
}

void PSCALRuntimeInterposeSetFeatureEnabled(int enabled) {
    gInterposeFeatureEnabled = enabled ? 1 : 0;
}

// --- Gates ---
static int pscalInterposeIsMainThread(void) {
    if (gInterposeMainThreadSet) {
        return pthread_equal(pthread_self(), gInterposeMainThread) ? 1 : 0;
    }
#if defined(__APPLE__)
    return pthread_main_np() ? 1 : 0;
#else
    return 0;
#endif
}

static int pscalInterposeEnabledFast(void) {
    if (pscalIsResolving()) return 0;
    if (!gInterposeMasterEnabled) return 0;
    if (!gInterposeFeatureEnabled) return 0;
    if (!gInterposeBootstrapped) return 0;
    if (gInterposeBypassDepth > 0) return 0;
    return 1;
}

static int pscalInterposeEnabledSlow(void) {
    if (pscalInterposeIsMainThread()) return 0;
    if (vprocInterposeBypassActive()) return 0;
    if (vprocThreadIsInterposeBypassed(pthread_self())) return 0;
    if (vprocInterposeReady() == 0 || vprocThreadHasActiveVproc() == 0) {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (!session || vprocSessionStdioIsDefault(session)) return 0;
    }
    return 1;
}

static int pscalInterposeEnter(void) {
    if (!pscalInterposeEnabledFast()) return 0;
    if (gInterposeGuardDepth > 0) return 0;
    gInterposeGuardDepth++;
    if (!pscalInterposeEnabledSlow()) {
        gInterposeGuardDepth--;
        return 0;
    }
    return 1;
}

static void pscalInterposeLeave(void) {
    if (gInterposeGuardDepth > 0) gInterposeGuardDepth--;
}

static const char *pscalInterposeAppBundlePrefix(void) {
    static char prefix[PATH_MAX];
    static atomic_int prefix_state = ATOMIC_VAR_INIT(0);
    if (atomic_load(&prefix_state) == 2) return prefix[0] ? prefix : NULL;
    int expected = 0;
    if (!atomic_compare_exchange_strong(&prefix_state, &expected, 1)) {
        while (atomic_load(&prefix_state) == 1) { }
        return prefix[0] ? prefix : NULL;
    }
    prefix[0] = '\0';
    uint32_t path_len = sizeof(prefix);
    char exec_path[PATH_MAX];
    if (_NSGetExecutablePath(exec_path, &path_len) != 0) {
        goto done;
    }
    exec_path[sizeof(exec_path) - 1] = '\0';
    const char *bundle_end = NULL;
    const char *contents = strstr(exec_path, "/Contents/MacOS/");
    if (contents) bundle_end = contents;
    else {
        const char *app = strstr(exec_path, ".app/");
        if (app) bundle_end = app + 4;
        else {
            const char *app_end = strstr(exec_path, ".app");
            if (app_end && app_end[4] == '\0') bundle_end = app_end + 4;
        }
    }
    if (!bundle_end || bundle_end <= exec_path) {
        goto done;
    }
    size_t bundle_len = (size_t)(bundle_end - exec_path);
    if (bundle_len >= sizeof(prefix)) {
        goto done;
    }
    memcpy(prefix, exec_path, bundle_len);
    prefix[bundle_len] = '\0';
done:
    atomic_store(&prefix_state, 2);
    return prefix[0] ? prefix : NULL;
}

static bool pscalInterposeShouldWrapThread(void *(*start_routine)(void *)) {
    if (!start_routine) return false;
    if (pscalIsResolving()) return false;
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr((void *)start_routine, &info) == 0 || !info.dli_fname) return false;
    const char *path = info.dli_fname;
    if (strncmp(path, "/System/Library/", 16) == 0) return false;
    if (strncmp(path, "/usr/lib/", 9) == 0) return false;
    if (strncmp(path, "/System/iOSSupport/", 19) == 0) return false;
    const char *bundle_prefix = pscalInterposeAppBundlePrefix();
    if (!bundle_prefix) return false;
    size_t prefix_len = strlen(bundle_prefix);
    if (prefix_len == 0) return false;
    if (strncmp(path, bundle_prefix, prefix_len) == 0 && (path[prefix_len] == '/' || path[prefix_len] == '\0')) {
        return true;
    }
    return false;
}

// --- Raw Wrapper Implementations ---

static ssize_t pscalRawRead(int fd, void *buf, size_t count) {
    static ssize_t (*fn)(int, void *, size_t) = NULL;
    if (fn && !pscalInterposeSymbolIsSystemLibrary((void *)fn)) {
        fn = NULL;
    }
    if (fn) { pscalInterposeEnterRaw(); ssize_t r = fn(fd, buf, count); pscalInterposeExitRaw(); return r; }
    
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (ssize_t)syscall(SYS_read, fd, buf, count);
#else
        if (buf && count > 0) memset(buf, 0, count);
        errno = EAGAIN;
        return -1;
#endif
    }
    if (!fn) fn = (ssize_t (*)(int, void *, size_t))pscalInterposeResolveKernel("__read_nocancel");
    if (!fn) fn = (ssize_t (*)(int, void *, size_t))pscalInterposeResolveKernel("read$NOCANCEL");
    if (!fn) fn = (ssize_t (*)(int, void *, size_t))pscalInterposeResolveKernel("__read");
    if (!fn) fn = (ssize_t (*)(int, void *, size_t))pscalInterposeResolveKernel("read");
    if (!fn) fn = (ssize_t (*)(int, void *, size_t))pscalInterposeResolveSystem("__read_nocancel");
    if (!fn) fn = (ssize_t (*)(int, void *, size_t))pscalInterposeResolveSystem("read$NOCANCEL");
    if (!fn) fn = (ssize_t (*)(int, void *, size_t))pscalInterposeResolveSystem("__read");
    if (!fn) fn = (ssize_t (*)(int, void *, size_t))pscalInterposeResolveSystem("read");
    
    if (fn) { pscalInterposeEnterRaw(); ssize_t r = fn(fd, buf, count); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (ssize_t)syscall(SYS_read, fd, buf, count);
#else
    errno = ENOSYS; return -1;
#endif
}

static ssize_t pscalRawWrite(int fd, const void *buf, size_t count) {
    static ssize_t (*fn)(int, const void *, size_t) = NULL;
    if (fn && !pscalInterposeSymbolIsSystemLibrary((void *)fn)) {
        fn = NULL;
    }
    if (fn && pscalInterposeSymbolIsLogRedirect((void *)fn)) {
        fn = NULL;
    }
    if (fn) { pscalInterposeEnterRaw(); ssize_t r = fn(fd, buf, count); pscalInterposeExitRaw(); return r; }

    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (ssize_t)syscall(SYS_write, fd, buf, count);
#else
        return count; // Fake success prevents malloc abort
#endif
    }
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveKernel("__write_nocancel");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveKernel("write$NOCANCEL");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveKernel("__write");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveKernel("write");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveSystem("__write_nocancel");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveSystem("write$NOCANCEL");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveSystem("__write");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveSystem("write");
    
    if (fn) { pscalInterposeEnterRaw(); ssize_t r = fn(fd, buf, count); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (ssize_t)syscall(SYS_write, fd, buf, count);
#else
    errno = ENOSYS; return -1;
#endif
}

static ssize_t pscalRawWriteKernel(int fd, const void *buf, size_t count) {
    static ssize_t (*fn)(int, const void *, size_t) = NULL;
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (ssize_t)syscall(SYS_write, fd, buf, count);
#else
        return count;
#endif
    }
    if (fn && !pscalInterposeSymbolIsSystemLibrary((void *)fn)) {
        fn = NULL;
    }
    if (fn && pscalInterposeSymbolIsLogRedirect((void *)fn)) {
        fn = NULL;
    }
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveKernel("__write_nocancel");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveKernel("write$NOCANCEL");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveKernel("__write");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveKernel("write");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveSystem("__write_nocancel");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveSystem("write$NOCANCEL");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveSystem("__write");
    if (!fn) fn = (ssize_t (*)(int, const void *, size_t))pscalInterposeResolveSystem("write");

    if (fn) {
        pscalInterposeEnterRaw();
        ssize_t r = fn(fd, buf, count);
        pscalInterposeExitRaw();
        return r;
    }
    if (gInterposeBootstrapped) {
        return pscalRawWrite(fd, buf, count);
    }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (ssize_t)syscall(SYS_write, fd, buf, count);
#else
    return count;
#endif
}

static ssize_t pscalRawWritev(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        const char *base = (const char *)iov[i].iov_base;
        size_t remaining = iov[i].iov_len;
        if (!base || remaining == 0) {
            continue;
        }
        while (remaining > 0) {
            size_t chunk = remaining;
            ssize_t w = pscalRawWrite(fd, base, chunk);
            if (w <= 0) {
                return (total > 0) ? total : w;
            }
            total += w;
            base += w;
            remaining -= (size_t)w;
            if ((size_t)w < chunk) {
                return total;
            }
        }
    }
    return total;
}

static ssize_t pscalRawReadv(int fd, const struct iovec *iov, int iovcnt) {
    if (!iov || iovcnt <= 0) {
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        void *base = iov[i].iov_base;
        size_t remaining = iov[i].iov_len;
        if (!base || remaining == 0) {
            continue;
        }
        ssize_t r = pscalRawRead(fd, base, remaining);
        if (r <= 0) {
            return (total > 0) ? total : r;
        }
        total += r;
        if ((size_t)r < remaining) {
            return total;
        }
    }
    return total;
}

static int pscalRawClose(int fd) {
    static int (*fn)(int) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fd); pscalInterposeExitRaw(); return r; }

    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_close, fd);
#else
        return 0; // Fake success to avoid descriptor leaks on Catalyst
#endif
    }
    if (!fn) fn = (int (*)(int))pscalInterposeResolveSystem("__close_nocancel");
    if (!fn) fn = (int (*)(int))pscalInterposeResolveSystem("close");
    
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fd); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_close, fd);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawDup(int fd) {
    static int (*fn)(int) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fd); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_dup, fd);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(int))pscalInterposeResolveSystem("dup");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fd); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_dup, fd);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawDup2(int fd, int target) {
    static int (*fn)(int, int) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fd, target); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_dup2, fd, target);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(int, int))pscalInterposeResolveSystem("dup2");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fd, target); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_dup2, fd, target);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawPipe(int pipefd[2]) {
    static int (*fn)(int[2]) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(pipefd); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_pipe, pipefd);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(int[2]))pscalInterposeResolveSystem("pipe");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(pipefd); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_pipe, pipefd);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawFstat(int fd, struct stat *st) {
    static int (*fn)(int, struct stat *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fd, st); pscalInterposeExitRaw(); return r; }

    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_fstat, fd, st);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(int, struct stat *))pscalInterposeResolveSystem("__fstat");
    if (!fn) fn = (int (*)(int, struct stat *))pscalInterposeResolveSystem("fstat");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fd, st); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_fstat, fd, st);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawStat(const char *path, struct stat *st) {
    static int (*fn)(const char *, struct stat *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path, st); pscalInterposeExitRaw(); return r; }

    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK && defined(AT_FDCWD)
        return syscall(SYS_fstatat, AT_FDCWD, path, st, 0);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *, struct stat *))pscalInterposeResolveSystem("__stat");
    if (!fn) fn = (int (*)(const char *, struct stat *))pscalInterposeResolveSystem("stat");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path, st); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK && defined(AT_FDCWD)
    return syscall(SYS_fstatat, AT_FDCWD, path, st, 0);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawLstat(const char *path, struct stat *st) {
    static int (*fn)(const char *, struct stat *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path, st); pscalInterposeExitRaw(); return r; }

    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK && defined(AT_FDCWD)
        return syscall(SYS_fstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *, struct stat *))pscalInterposeResolveSystem("__lstat");
    if (!fn) fn = (int (*)(const char *, struct stat *))pscalInterposeResolveSystem("lstat");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path, st); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK && defined(AT_FDCWD)
    return syscall(SYS_fstatat, AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawChdir(const char *path) {
    static int (*fn)(const char *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_chdir, path);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *))pscalInterposeResolveSystem("chdir");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_chdir, path);
#else
    errno = ENOSYS; return -1;
#endif
}

static char *pscalRawGetcwd(char *buf, size_t size) {
    static char *(*fn)(char *, size_t) = NULL;
    if (fn) { pscalInterposeEnterRaw(); char *r = fn(buf, size); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
        errno = ENOSYS; return NULL;
    }
    if (!fn) fn = (char *(*)(char *, size_t))pscalInterposeResolveSystem("getcwd");
    if (fn) { pscalInterposeEnterRaw(); char *r = fn(buf, size); pscalInterposeExitRaw(); return r; }
    errno = ENOSYS; return NULL;
}

static int pscalRawAccess(const char *path, int mode) {
    static int (*fn)(const char *, int) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path, mode); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_access, path, mode);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *, int))pscalInterposeResolveSystem("__access");
    if (!fn) fn = (int (*)(const char *, int))pscalInterposeResolveSystem("access");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path, mode); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_access, path, mode);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawMkdir(const char *path, mode_t mode) {
    static int (*fn)(const char *, mode_t) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path, mode); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_mkdir, path, mode);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *, mode_t))pscalInterposeResolveSystem("mkdir");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path, mode); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_mkdir, path, mode);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawRmdir(const char *path) {
    static int (*fn)(const char *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_rmdir, path);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *))pscalInterposeResolveSystem("rmdir");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_rmdir, path);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawUnlink(const char *path) {
    static int (*fn)(const char *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_unlink, path);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *))pscalInterposeResolveSystem("unlink");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_unlink, path);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawRemove(const char *path) {
    static int (*fn)(const char *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
        errno = ENOSYS; return -1;
    }
    if (!fn) fn = (int (*)(const char *))pscalInterposeResolveSystem("remove");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(path); pscalInterposeExitRaw(); return r; }
    errno = ENOSYS; return -1;
}

static int pscalRawRename(const char *oldpath, const char *newpath) {
    static int (*fn)(const char *, const char *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(oldpath, newpath); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_rename, oldpath, newpath);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *, const char *))pscalInterposeResolveSystem("rename");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(oldpath, newpath); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_rename, oldpath, newpath);
#else
    errno = ENOSYS; return -1;
#endif
}

static DIR *pscalRawOpendir(const char *name) {
    static DIR *(*fn)(const char *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); DIR *r = fn(name); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) { errno = ENOSYS; return NULL; }
    if (!fn) fn = (DIR *(*)(const char *))pscalInterposeResolveSystem("opendir");
    if (fn) { pscalInterposeEnterRaw(); DIR *r = fn(name); pscalInterposeExitRaw(); return r; }
    errno = ENOSYS; return NULL;
}

static int pscalRawSymlink(const char *target, const char *linkpath) {
    static int (*fn)(const char *, const char *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(target, linkpath); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_symlink, target, linkpath);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *, const char *))pscalInterposeResolveSystem("symlink");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(target, linkpath); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_symlink, target, linkpath);
#else
    errno = ENOSYS; return -1;
#endif
}

static ssize_t pscalRawReadlink(const char *path, char *buf, size_t size) {
    static ssize_t (*fn)(const char *, char *, size_t) = NULL;
    if (fn) { pscalInterposeEnterRaw(); ssize_t r = fn(path, buf, size); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (ssize_t)syscall(SYS_readlink, path, buf, size);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (ssize_t (*)(const char *, char *, size_t))pscalInterposeResolveSystem("readlink");
    if (fn) { pscalInterposeEnterRaw(); ssize_t r = fn(path, buf, size); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (ssize_t)syscall(SYS_readlink, path, buf, size);
#else
    errno = ENOSYS; return -1;
#endif
}

static char *pscalRawRealpath(const char *path, char *resolved_path) {
    static char *(*fn)(const char *, char *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); char *r = fn(path, resolved_path); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) { errno = ENOSYS; return NULL; }
    if (!fn) fn = (char *(*)(const char *, char *))pscalInterposeResolveSystem("realpath");
    if (fn) { pscalInterposeEnterRaw(); char *r = fn(path, resolved_path); pscalInterposeExitRaw(); return r; }
    errno = ENOSYS; return NULL;
}

// FIX: Rewritten to be a true RAW wrapper (No Enter checks inside)
static int pscalRawIoctl(int fd, unsigned long request, ...) {
    void *arg = NULL;
    va_list ap;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);

    static int (*fn)(int, unsigned long, ...) = NULL;
    
    // 1. If we have the pointer, call it directly (bypassing the interposer)
    if (fn) {
        pscalInterposeEnterRaw();
        int res = fn(fd, request, arg);
        pscalInterposeExitRaw();
        return res;
    }

    // 2. If we are resolving, use syscall fallback
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_ioctl, fd, request, arg);
#else
        errno = ENOSYS; return -1;
#endif
    }

    // 3. Resolve the symbol
    if (!fn) fn = (int (*)(int, unsigned long, ...))pscalInterposeResolveSystem("ioctl");

    // 4. Call it
    if (fn) {
        pscalInterposeEnterRaw();
        int res = fn(fd, request, arg);
        pscalInterposeExitRaw();
        return res;
    }

    // 5. Final fallback
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_ioctl, fd, request, arg);
#else
    errno = ENOSYS; return -1;
#endif
}

static off_t pscalRawLseek(int fd, off_t offset, int whence) {
    static off_t (*fn)(int, off_t, int) = NULL;
    if (fn) { pscalInterposeEnterRaw(); off_t r = fn(fd, offset, whence); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (off_t)syscall(SYS_lseek, fd, offset, whence);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (off_t (*)(int, off_t, int))pscalInterposeResolveSystem("lseek");
    if (fn) { pscalInterposeEnterRaw(); off_t r = fn(fd, offset, whence); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (off_t)syscall(SYS_lseek, fd, offset, whence);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawIsatty(int fd) {
    struct termios term;
    // FIX: Calls pscalRawIoctl, which is now safe from recursion
    if (pscalRawIoctl(fd, (unsigned long)TIOCGETA, &term) == 0) return 1;
    return 0;
}

static int pscalRawPoll(struct pollfd *fds, nfds_t nfds, int timeout) {
    static int (*fn)(struct pollfd *, nfds_t, int) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fds, nfds, timeout); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) { errno = ENOSYS; return -1; }
    if (!fn) fn = (int (*)(struct pollfd *, nfds_t, int))pscalInterposeResolveSystem("poll");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(fds, nfds, timeout); pscalInterposeExitRaw(); return r; }
    errno = ENOSYS; return -1;
}

static int pscalRawSelect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    static int (*fn)(int, fd_set *, fd_set *, fd_set *, struct timeval *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(nfds, readfds, writefds, exceptfds, timeout); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_select, nfds, readfds, writefds, exceptfds, timeout);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(int, fd_set *, fd_set *, fd_set *, struct timeval *))pscalInterposeResolveSystem("select");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(nfds, readfds, writefds, exceptfds, timeout); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_select, nfds, readfds, writefds, exceptfds, timeout);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawOpen(const char *path, int flags, int mode, int has_mode) {
    static int (*fn)(const char *, int, ...) = NULL;
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_open, path, flags, mode);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const char *, int, ...))pscalInterposeResolveSystem("__open_nocancel");
    if (!fn) fn = (int (*)(const char *, int, ...))pscalInterposeResolveSystem("__open");
    if (!fn) fn = (int (*)(const char *, int, ...))pscalInterposeResolveSystem("open");
    if (fn) {
        pscalInterposeEnterRaw();
        int ret = has_mode ? fn(path, flags, mode) : fn(path, flags);
        pscalInterposeExitRaw();
        return ret;
    }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_open, path, flags, mode);
#else
    errno = ENOSYS; return -1;
#endif
}

static pid_t pscalRawWaitpid(pid_t pid, int *status, int options) {
    static pid_t (*fn)(pid_t, int *, int) = NULL;
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(pid, status, options); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (pid_t)syscall(SYS_wait4, pid, status, options, NULL);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (pid_t (*)(pid_t, int *, int))pscalInterposeResolveSystem("waitpid");
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(pid, status, options); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (pid_t)syscall(SYS_wait4, pid, status, options, NULL);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawKill(pid_t pid, int sig) {
    static int (*fn)(pid_t, int) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(pid, sig); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_kill, pid, sig);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(pid_t, int))pscalInterposeResolveSystem("kill");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(pid, sig); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_kill, pid, sig);
#else
    errno = ENOSYS; return -1;
#endif
}

static pid_t pscalRawGetpid(void) {
    static pid_t (*fn)(void) = NULL;
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (pid_t)syscall(SYS_getpid);
#else
        return 0; // Fake success on Catalyst
#endif
    }
    if (!fn) fn = (pid_t (*)(void))pscalInterposeResolveSystem("getpid");
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (pid_t)syscall(SYS_getpid);
#else
    errno = ENOSYS; return -1;
#endif
}

static pid_t pscalRawGetppid(void) {
    static pid_t (*fn)(void) = NULL;
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (pid_t)syscall(SYS_getppid);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (pid_t (*)(void))pscalInterposeResolveSystem("getppid");
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (pid_t)syscall(SYS_getppid);
#else
    errno = ENOSYS; return -1;
#endif
}

static pid_t pscalRawGetpgrp(void) {
    static pid_t (*fn)(void) = NULL;
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (pid_t)syscall(SYS_getpgrp);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (pid_t (*)(void))pscalInterposeResolveSystem("getpgrp");
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (pid_t)syscall(SYS_getpgrp);
#else
    errno = ENOSYS; return -1;
#endif
}

static pid_t pscalRawGetpgid(pid_t pid) {
    static pid_t (*fn)(pid_t) = NULL;
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(pid); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (pid_t)syscall(SYS_getpgid, pid);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (pid_t (*)(pid_t))pscalInterposeResolveSystem("getpgid");
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(pid); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (pid_t)syscall(SYS_getpgid, pid);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawSetpgid(pid_t pid, pid_t pgid) {
    static int (*fn)(pid_t, pid_t) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(pid, pgid); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_setpgid, pid, pgid);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(pid_t, pid_t))pscalInterposeResolveSystem("setpgid");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(pid, pgid); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_setpgid, pid, pgid);
#else
    errno = ENOSYS; return -1;
#endif
}

static pid_t pscalRawGetsid(pid_t pid) {
    static pid_t (*fn)(pid_t) = NULL;
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(pid); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (pid_t)syscall(SYS_getsid, pid);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (pid_t (*)(pid_t))pscalInterposeResolveSystem("getsid");
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(pid); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (pid_t)syscall(SYS_getsid, pid);
#else
    errno = ENOSYS; return -1;
#endif
}

static pid_t pscalRawSetsid(void) {
    static pid_t (*fn)(void) = NULL;
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (pid_t)syscall(SYS_setsid);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (pid_t (*)(void))pscalInterposeResolveSystem("setsid");
    if (fn) { pscalInterposeEnterRaw(); pid_t r = fn(); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (pid_t)syscall(SYS_setsid);
#else
    errno = ENOSYS; return -1;
#endif
}

static pid_t pscalRawTcgetpgrp(int fd) {
    pid_t pgid = -1;
    // FIX: Uses safe pscalRawIoctl
    if (pscalRawIoctl(fd, (unsigned long)TIOCGPGRP, &pgid) == 0) return pgid;
    return (pid_t)-1;
}

static int pscalRawTcsetpgrp(int fd, pid_t pgid) {
    return pscalRawIoctl(fd, (unsigned long)TIOCSPGRP, &pgid);
}

// FIX: Added EnterRaw/ExitRaw guards to pthread_create
static int pscalRawPthreadCreate(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
    static int (*fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = NULL;
    if (fn) { 
        pscalInterposeEnterRaw(); 
        int r = fn(thread, attr, start_routine, arg); 
        pscalInterposeExitRaw(); 
        return r; 
    }
    if (pscalIsResolving()) return EINVAL;
    if (!fn) fn = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))pscalInterposeResolveGeneric("pthread_create", RTLD_NEXT);
    if (!fn) fn = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))pscalInterposeResolveSystem("pthread_create");
    if (fn) { 
        pscalInterposeEnterRaw(); 
        int r = fn(thread, attr, start_routine, arg); 
        pscalInterposeExitRaw(); 
        return r; 
    }
    return EINVAL;
}

static int pscalRawSigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    static int (*fn)(int, const struct sigaction *, struct sigaction *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(sig, act, oldact); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_sigaction, sig, act, oldact);
#else
        if (oldact) memset(oldact, 0, sizeof(*oldact));
        errno = EAGAIN;
        return -1;
#endif
    }
    if (!fn) fn = (int (*)(int, const struct sigaction *, struct sigaction *))pscalInterposeResolveSystem("sigaction");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(sig, act, oldact); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_sigaction, sig, act, oldact);
#else
    if (oldact) memset(oldact, 0, sizeof(*oldact));
    errno = ENOSYS;
    return -1;
#endif
}

static int pscalRawSigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    static int (*fn)(int, const sigset_t *, sigset_t *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(how, set, oldset); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_sigprocmask, how, set, oldset);
#else
        if (oldset) memset(oldset, 0, sizeof(*oldset));
        errno = EAGAIN;
        return -1;
#endif
    }
    if (!fn) fn = (int (*)(int, const sigset_t *, sigset_t *))pscalInterposeResolveSystem("sigprocmask");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(how, set, oldset); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_sigprocmask, how, set, oldset);
#else
    if (oldset) memset(oldset, 0, sizeof(*oldset));
    errno = ENOSYS;
    return -1;
#endif
}

static int pscalRawSigpending(sigset_t *set) {
    static int (*fn)(sigset_t *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(set); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_sigpending, set);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(sigset_t *))pscalInterposeResolveSystem("sigpending");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(set); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_sigpending, set);
#else
    errno = ENOSYS; return -1;
#endif
}

static int pscalRawSigsuspend(const sigset_t *mask) {
    static int (*fn)(const sigset_t *) = NULL;
    if (fn) { pscalInterposeEnterRaw(); int r = fn(mask); pscalInterposeExitRaw(); return r; }
    if (pscalIsResolving()) {
#if PSCAL_ENABLE_SYSCALL_FALLBACK
        return (int)syscall(SYS_sigsuspend, mask);
#else
        errno = ENOSYS; return -1;
#endif
    }
    if (!fn) fn = (int (*)(const sigset_t *))pscalInterposeResolveSystem("sigsuspend");
    if (fn) { pscalInterposeEnterRaw(); int r = fn(mask); pscalInterposeExitRaw(); return r; }
#if PSCAL_ENABLE_SYSCALL_FALLBACK
    return (int)syscall(SYS_sigsuspend, mask);
#else
    errno = ENOSYS; return -1;
#endif
}

static VProcSigHandler pscalRawSignal(int sig, VProcSigHandler handler) {
    struct sigaction sa;
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (pscalRawSigaction(sig, &sa, &old) != 0) return SIG_ERR;
    return old.sa_handler;
}

static int pscalRawRaise(int sig) {
    pid_t pid = pscalRawGetpid();
    if (pid < 0) return -1;
    return pscalRawKill(pid, sig);
}

static int pscalRawPthreadSigmask(int how, const sigset_t *set, sigset_t *oldset) {
    return pscalRawSigprocmask(how, set, oldset);
}

// --- Interposer Wrappers ---

// Undefine syscall names to prevent macro expansion issues if unistd.h defined them
#undef read
#undef write
#undef close
#undef dup
#undef dup2
#undef pipe
#undef fstat
#undef stat
#undef lstat
#undef chdir
#undef getcwd
#undef access
#undef mkdir
#undef rmdir
#undef unlink
#undef remove
#undef rename
#undef opendir
#undef symlink
#undef readlink
#undef realpath
#undef ioctl
#undef lseek
#undef isatty
#undef poll
#undef select
#undef readv
#undef writev
#undef open
#undef waitpid
#undef kill
#undef getpid
#undef getppid
#undef getpgrp
#undef getpgid
#undef setpgid
#undef getsid
#undef setsid
#undef tcgetpgrp
#undef tcsetpgrp
#undef pthread_create
#undef sigaction
#undef sigprocmask
#undef sigpending
#undef sigsuspend
#undef signal
#undef raise
#undef pthread_sigmask

static ssize_t pscal_interpose_read(int fd, void *buf, size_t count) {
    if (!pscalInterposeEnter()) return pscalRawRead(fd, buf, count);
    ssize_t res = vprocReadShim(fd, buf, count);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_read, read);
PSCAL_DYLD_INTERPOSE(pscal_interpose_read, __read);
PSCAL_DYLD_INTERPOSE(pscal_interpose_read, __read_nocancel);

static ssize_t pscal_interpose_write(int fd, const void *buf, size_t count) {
    if (pscalInterposeCallerIsLogRedirect()) {
        return pscalRawWriteKernel(fd, buf, count);
    }
    if (pscalInterposeBypassActive()) {
        return pscalRawWriteKernel(fd, buf, count);
    }
    if (!pscalInterposeEnter()) {
        return pscalRawWriteKernel(fd, buf, count);
    }
    ssize_t res = vprocWriteShim(fd, buf, count);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_write, write);
PSCAL_DYLD_INTERPOSE(pscal_interpose_write, __write);
PSCAL_DYLD_INTERPOSE(pscal_interpose_write, __write_nocancel);

static ssize_t pscal_interpose_readv(int fd, const struct iovec *iov, int iovcnt) {
    if (!pscalInterposeEnter()) return pscalRawReadv(fd, iov, iovcnt);
    if (!iov || iovcnt <= 0) {
        pscalInterposeLeave();
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) {
            continue;
        }
        ssize_t r = vprocReadShim(fd, iov[i].iov_base, iov[i].iov_len);
        if (r <= 0) {
            pscalInterposeLeave();
            return (total > 0) ? total : r;
        }
        total += r;
        if ((size_t)r < iov[i].iov_len) {
            pscalInterposeLeave();
            return total;
        }
    }
    pscalInterposeLeave();
    return total;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_readv, readv);
PSCAL_DYLD_INTERPOSE(pscal_interpose_readv, __readv);
PSCAL_DYLD_INTERPOSE(pscal_interpose_readv, __readv_nocancel);

static ssize_t pscal_interpose_writev(int fd, const struct iovec *iov, int iovcnt) {
    if (pscalInterposeCallerIsLogRedirect()) {
        return pscalRawWritev(fd, iov, iovcnt);
    }
    if (pscalInterposeBypassActive()) {
        return pscalRawWritev(fd, iov, iovcnt);
    }
    if (!pscalInterposeEnter()) {
        return pscalRawWritev(fd, iov, iovcnt);
    }
    if (!iov || iovcnt <= 0) {
        pscalInterposeLeave();
        errno = EINVAL;
        return -1;
    }
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; ++i) {
        const char *base = (const char *)iov[i].iov_base;
        size_t remaining = iov[i].iov_len;
        if (!base || remaining == 0) {
            continue;
        }
        while (remaining > 0) {
            size_t chunk = remaining;
            ssize_t w = vprocWriteShim(fd, base, chunk);
            if (w <= 0) {
                pscalInterposeLeave();
                return (total > 0) ? total : w;
            }
            total += w;
            base += w;
            remaining -= (size_t)w;
            if ((size_t)w < chunk) {
                pscalInterposeLeave();
                return total;
            }
        }
    }
    pscalInterposeLeave();
    return total;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_writev, writev);
PSCAL_DYLD_INTERPOSE(pscal_interpose_writev, __writev);
PSCAL_DYLD_INTERPOSE(pscal_interpose_writev, __writev_nocancel);

static int pscal_interpose_close(int fd) {
    if (!pscalInterposeEnter()) return pscalRawClose(fd);
    int res = vprocCloseShim(fd);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_close, close);
PSCAL_DYLD_INTERPOSE(pscal_interpose_close, __close);
PSCAL_DYLD_INTERPOSE(pscal_interpose_close, __close_nocancel);

static int pscal_interpose_dup(int fd) {
    if (!pscalInterposeEnter()) return pscalRawDup(fd);
    int res = vprocDupShim(fd);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_dup, dup);

static int pscal_interpose_dup2(int fd, int target) {
    if (!pscalInterposeEnter()) return pscalRawDup2(fd, target);
    int res = vprocDup2Shim(fd, target);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_dup2, dup2);

static int pscal_interpose_pipe(int pipefd[2]) {
    if (!pscalInterposeEnter()) return pscalRawPipe(pipefd);
    int res = vprocPipeShim(pipefd);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_pipe, pipe);

static int pscal_interpose_fstat(int fd, struct stat *st) {
    if (!pscalInterposeEnter()) return pscalRawFstat(fd, st);
    int res = vprocFstatShim(fd, st);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_fstat, fstat);

static int pscal_interpose_stat(const char *path, struct stat *st) {
    if (!pscalInterposeEnter()) return pscalRawStat(path, st);
    int res = vprocStatShim(path, st);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_stat, stat);

static int pscal_interpose_lstat(const char *path, struct stat *st) {
    if (!pscalInterposeEnter()) return pscalRawLstat(path, st);
    int res = vprocLstatShim(path, st);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_lstat, lstat);

static int pscal_interpose_chdir(const char *path) {
    if (!pscalInterposeEnter()) return pscalRawChdir(path);
    int res = vprocChdirShim(path);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_chdir, chdir);

static char *pscal_interpose_getcwd(char *buf, size_t size) {
    if (!pscalInterposeEnter()) return pscalRawGetcwd(buf, size);
    char *res = vprocGetcwdShim(buf, size);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_getcwd, getcwd);

static int pscal_interpose_access(const char *path, int mode) {
    if (!pscalInterposeEnter()) return pscalRawAccess(path, mode);
    int res = vprocAccessShim(path, mode);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_access, access);

static int pscal_interpose_mkdir(const char *path, mode_t mode) {
    if (!pscalInterposeEnter()) return pscalRawMkdir(path, mode);
    int res = vprocMkdirShim(path, mode);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_mkdir, mkdir);

static int pscal_interpose_rmdir(const char *path) {
    if (!pscalInterposeEnter()) return pscalRawRmdir(path);
    int res = vprocRmdirShim(path);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_rmdir, rmdir);

static int pscal_interpose_unlink(const char *path) {
    if (!pscalInterposeEnter()) return pscalRawUnlink(path);
    int res = vprocUnlinkShim(path);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_unlink, unlink);

static int pscal_interpose_remove(const char *path) {
    if (!pscalInterposeEnter()) return pscalRawRemove(path);
    int res = vprocRemoveShim(path);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_remove, remove);

static int pscal_interpose_rename(const char *oldpath, const char *newpath) {
    if (!pscalInterposeEnter()) return pscalRawRename(oldpath, newpath);
    int res = vprocRenameShim(oldpath, newpath);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_rename, rename);

static DIR *pscal_interpose_opendir(const char *name) {
    if (!pscalInterposeEnter()) return pscalRawOpendir(name);
    DIR *res = vprocOpendirShim(name);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_opendir, opendir);

static int pscal_interpose_symlink(const char *target, const char *linkpath) {
    if (!pscalInterposeEnter()) return pscalRawSymlink(target, linkpath);
    int res = vprocSymlinkShim(target, linkpath);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_symlink, symlink);

static ssize_t pscal_interpose_readlink(const char *path, char *buf, size_t size) {
    if (!pscalInterposeEnter()) return pscalRawReadlink(path, buf, size);
    ssize_t res = vprocReadlinkShim(path, buf, size);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_readlink, readlink);

static char *pscal_interpose_realpath(const char *path, char *resolved_path) {
    if (!pscalInterposeEnter()) return pscalRawRealpath(path, resolved_path);
    char *res = vprocRealpathShim(path, resolved_path);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_realpath, realpath);

static int pscal_interpose_ioctl(int fd, unsigned long request, ...) {
    void *arg = NULL;
    va_list ap;
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (!pscalInterposeEnter()) return pscalRawIoctl(fd, request, arg);
    int res = vprocIoctlShim(fd, request, arg);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_ioctl, ioctl);

static off_t pscal_interpose_lseek(int fd, off_t offset, int whence) {
    if (!pscalInterposeEnter()) return pscalRawLseek(fd, offset, whence);
    off_t res = vprocLseekShim(fd, offset, whence);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_lseek, lseek);

static int pscal_interpose_isatty(int fd) {
    if (!pscalInterposeEnter()) return pscalRawIsatty(fd);
    int res = vprocIsattyShim(fd);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_isatty, isatty);

static int pscal_interpose_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (!pscalInterposeEnter()) return pscalRawPoll(fds, nfds, timeout);
    int res = vprocPollShim(fds, nfds, timeout);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_poll, poll);

static int pscal_interpose_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    if (!pscalInterposeEnter()) return pscalRawSelect(nfds, readfds, writefds, exceptfds, timeout);
    int res = vprocSelectShim(nfds, readfds, writefds, exceptfds, timeout);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_select, select);

static int pscal_interpose_open(const char *path, int flags, ...) {
    int mode = 0;
    int has_mode = (flags & O_CREAT) != 0;
    if (has_mode) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    if (!pscalInterposeEnter()) return pscalRawOpen(path, flags, mode, has_mode);
    int res = has_mode ? vprocOpenShim(path, flags, mode) : vprocOpenShim(path, flags);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_open, open);
PSCAL_DYLD_INTERPOSE(pscal_interpose_open, __open);
PSCAL_DYLD_INTERPOSE(pscal_interpose_open, __open_nocancel);

static pid_t pscal_interpose_waitpid(pid_t pid, int *status, int options) {
    if (!pscalInterposeEnter()) return pscalRawWaitpid(pid, status, options);
    pid_t res = vprocWaitPidShim(pid, status, options);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_waitpid, waitpid);

static int pscal_interpose_kill(pid_t pid, int sig) {
    if (!pscalInterposeEnter()) return pscalRawKill(pid, sig);
    int res = vprocKillShim(pid, sig);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_kill, kill);

static pid_t pscal_interpose_getpid(void) {
    if (!pscalInterposeEnter()) return pscalRawGetpid();
    pid_t res = vprocGetPidShim();
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_getpid, getpid);

static pid_t pscal_interpose_getppid(void) {
    if (!pscalInterposeEnter()) return pscalRawGetppid();
    pid_t res = vprocGetPpidShim();
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_getppid, getppid);

static pid_t pscal_interpose_getpgrp(void) {
    if (!pscalInterposeEnter()) return pscalRawGetpgrp();
    pid_t res = vprocGetpgrpShim();
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_getpgrp, getpgrp);

static pid_t pscal_interpose_getpgid(pid_t pid) {
    if (!pscalInterposeEnter()) return pscalRawGetpgid(pid);
    pid_t res = vprocGetpgidShim(pid);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_getpgid, getpgid);

static int pscal_interpose_setpgid(pid_t pid, pid_t pgid) {
    if (!pscalInterposeEnter()) return pscalRawSetpgid(pid, pgid);
    int res = vprocSetpgidShim(pid, pgid);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_setpgid, setpgid);

static pid_t pscal_interpose_getsid(pid_t pid) {
    if (!pscalInterposeEnter()) return pscalRawGetsid(pid);
    pid_t res = vprocGetsidShim(pid);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_getsid, getsid);

static pid_t pscal_interpose_setsid(void) {
    if (!pscalInterposeEnter()) return pscalRawSetsid();
    pid_t res = vprocSetsidShim();
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_setsid, setsid);

static pid_t pscal_interpose_tcgetpgrp(int fd) {
    if (!pscalInterposeEnter()) return pscalRawTcgetpgrp(fd);
    pid_t res = vprocTcgetpgrpShim(fd);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_tcgetpgrp, tcgetpgrp);

static int pscal_interpose_tcsetpgrp(int fd, pid_t pgid) {
    if (!pscalInterposeEnter()) return pscalRawTcsetpgrp(fd, pgid);
    int res = vprocTcsetpgrpShim(fd, pgid);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_tcsetpgrp, tcsetpgrp);

static int pscal_interpose_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg) {
    if (!pscalInterposeEnter()) return pscalRawPthreadCreate(thread, attr, start_routine, arg);
    if (!pscalInterposeShouldWrapThread(start_routine)) {
        pscalInterposeLeave();
        return pscalRawPthreadCreate(thread, attr, start_routine, arg);
    }
    int res = vprocPthreadCreateShim(thread, attr, start_routine, arg);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_pthread_create, pthread_create);

static int pscal_interpose_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    if (!pscalInterposeEnter()) return pscalRawSigaction(sig, act, oldact);
    int res = vprocSigactionShim(sig, act, oldact);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_sigaction, sigaction);

static int pscal_interpose_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (!pscalInterposeEnter()) return pscalRawSigprocmask(how, set, oldset);
    int res = vprocSigprocmaskShim(how, set, oldset);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_sigprocmask, sigprocmask);

static int pscal_interpose_sigpending(sigset_t *set) {
    if (!pscalInterposeEnter()) return pscalRawSigpending(set);
    int res = vprocSigpendingShim(set);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_sigpending, sigpending);

static int pscal_interpose_sigsuspend(const sigset_t *mask) {
    if (!pscalInterposeEnter()) return pscalRawSigsuspend(mask);
    int res = vprocSigsuspendShim(mask);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_sigsuspend, sigsuspend);

static VProcSigHandler pscal_interpose_signal(int sig, VProcSigHandler handler) {
    if (!pscalInterposeEnter()) return pscalRawSignal(sig, handler);
    VProcSigHandler res = vprocSignalShim(sig, handler);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_signal, signal);

static int pscal_interpose_raise(int sig) {
    if (!pscalInterposeEnter()) return pscalRawRaise(sig);
    int res = vprocRaiseShim(sig);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_raise, raise);

static int pscal_interpose_pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) {
    if (!pscalInterposeEnter()) return pscalRawPthreadSigmask(how, set, oldset);
    int res = vprocPthreadSigmaskShim(how, set, oldset);
    pscalInterposeLeave();
    return res;
}
PSCAL_DYLD_INTERPOSE(pscal_interpose_pthread_sigmask, pthread_sigmask);

static void pscalInterposeInstallHooks(void) {
#if defined(__APPLE__) && !PSCAL_USE_DYLD_INTERPOSE
    static int installed = 0;
    if (installed) return;
    installed = 1;
    static struct pscalRebinding rebindings[] = {
        { "__read_nocancel", (void *)pscal_interpose_read, NULL },
        { "__write_nocancel", (void *)pscal_interpose_write, NULL },
        { "__readv_nocancel", (void *)pscal_interpose_readv, NULL },
        { "__writev_nocancel", (void *)pscal_interpose_writev, NULL },
        { "__read$UNIX2003", (void *)pscal_interpose_read, NULL },
        { "__write$UNIX2003", (void *)pscal_interpose_write, NULL },
        { "__readv$UNIX2003", (void *)pscal_interpose_readv, NULL },
        { "__writev$UNIX2003", (void *)pscal_interpose_writev, NULL },
        { "__close$UNIX2003", (void *)pscal_interpose_close, NULL },
        { "__open$UNIX2003", (void *)pscal_interpose_open, NULL },
        { "__read", (void *)pscal_interpose_read, NULL },
        { "__write", (void *)pscal_interpose_write, NULL },
        { "__readv", (void *)pscal_interpose_readv, NULL },
        { "__writev", (void *)pscal_interpose_writev, NULL },
        { "__close_nocancel", (void *)pscal_interpose_close, NULL },
        { "__close", (void *)pscal_interpose_close, NULL },
        { "__open_nocancel", (void *)pscal_interpose_open, NULL },
        { "__open", (void *)pscal_interpose_open, NULL },
        { "read$NOCANCEL", (void *)pscal_interpose_read, NULL },
        { "write$NOCANCEL", (void *)pscal_interpose_write, NULL },
        { "close$NOCANCEL", (void *)pscal_interpose_close, NULL },
        { "read$UNIX2003", (void *)pscal_interpose_read, NULL },
        { "write$UNIX2003", (void *)pscal_interpose_write, NULL },
        { "readv$UNIX2003", (void *)pscal_interpose_readv, NULL },
        { "writev$UNIX2003", (void *)pscal_interpose_writev, NULL },
        { "close$UNIX2003", (void *)pscal_interpose_close, NULL },
        { "open$UNIX2003", (void *)pscal_interpose_open, NULL },
        { "read", (void *)pscal_interpose_read, NULL },
        { "write", (void *)pscal_interpose_write, NULL },
        { "readv", (void *)pscal_interpose_readv, NULL },
        { "writev", (void *)pscal_interpose_writev, NULL },
        { "close", (void *)pscal_interpose_close, NULL },
        { "dup", (void *)pscal_interpose_dup, NULL },
        { "dup2", (void *)pscal_interpose_dup2, NULL },
        { "pipe", (void *)pscal_interpose_pipe, NULL },
        { "fstat", (void *)pscal_interpose_fstat, NULL },
        { "stat", (void *)pscal_interpose_stat, NULL },
        { "lstat", (void *)pscal_interpose_lstat, NULL },
        { "chdir", (void *)pscal_interpose_chdir, NULL },
        { "getcwd", (void *)pscal_interpose_getcwd, NULL },
        { "access", (void *)pscal_interpose_access, NULL },
        { "mkdir", (void *)pscal_interpose_mkdir, NULL },
        { "rmdir", (void *)pscal_interpose_rmdir, NULL },
        { "unlink", (void *)pscal_interpose_unlink, NULL },
        { "remove", (void *)pscal_interpose_remove, NULL },
        { "rename", (void *)pscal_interpose_rename, NULL },
        { "opendir", (void *)pscal_interpose_opendir, NULL },
        { "symlink", (void *)pscal_interpose_symlink, NULL },
        { "readlink", (void *)pscal_interpose_readlink, NULL },
        { "realpath", (void *)pscal_interpose_realpath, NULL },
        { "ioctl", (void *)pscal_interpose_ioctl, NULL },
        { "lseek", (void *)pscal_interpose_lseek, NULL },
        { "isatty", (void *)pscal_interpose_isatty, NULL },
        { "poll", (void *)pscal_interpose_poll, NULL },
        { "select", (void *)pscal_interpose_select, NULL },
        { "open", (void *)pscal_interpose_open, NULL },
        { "waitpid", (void *)pscal_interpose_waitpid, NULL },
        { "kill", (void *)pscal_interpose_kill, NULL },
        { "getpid", (void *)pscal_interpose_getpid, NULL },
        { "getppid", (void *)pscal_interpose_getppid, NULL },
        { "getpgrp", (void *)pscal_interpose_getpgrp, NULL },
        { "getpgid", (void *)pscal_interpose_getpgid, NULL },
        { "setpgid", (void *)pscal_interpose_setpgid, NULL },
        { "getsid", (void *)pscal_interpose_getsid, NULL },
        { "setsid", (void *)pscal_interpose_setsid, NULL },
        { "tcgetpgrp", (void *)pscal_interpose_tcgetpgrp, NULL },
        { "tcsetpgrp", (void *)pscal_interpose_tcsetpgrp, NULL },
        { "pthread_create", (void *)pscal_interpose_pthread_create, NULL },
        { "sigaction", (void *)pscal_interpose_sigaction, NULL },
        { "sigprocmask", (void *)pscal_interpose_sigprocmask, NULL },
        { "sigpending", (void *)pscal_interpose_sigpending, NULL },
        { "sigsuspend", (void *)pscal_interpose_sigsuspend, NULL },
        { "signal", (void *)pscal_interpose_signal, NULL },
        { "raise", (void *)pscal_interpose_raise, NULL },
        { "pthread_sigmask", (void *)pscal_interpose_pthread_sigmask, NULL }
    };
    (void)pscalRebindSymbols(rebindings, sizeof(rebindings) / sizeof(rebindings[0]));
#endif
}
