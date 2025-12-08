#include <netdb.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>

#ifndef PSCAL_TARGET_IOS
#error "hosts_override.c should only be compiled for iOS builds"
#endif

#define PSCAL_HOSTS_CUSTOM_IMPL
#include "common/pscal_hosts.h"

// Simple override for getaddrinfo that consults $PSCALI_CONTAINER_ROOT/etc/hosts
// before deferring to the system resolver. This keeps name lookups working on
// iOS where /etc/hosts cannot be modified.

typedef int (*system_getaddrinfo_fn)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
typedef void (*system_freeaddrinfo_fn)(struct addrinfo *);

static system_getaddrinfo_fn resolve_system_getaddrinfo(void) {
    static system_getaddrinfo_fn fn = NULL;
    if (!fn) {
        fn = (system_getaddrinfo_fn)dlsym(RTLD_NEXT, "getaddrinfo");
    }
    if (!fn) {
        fn = (system_getaddrinfo_fn)dlsym(RTLD_DEFAULT, "getaddrinfo");
    }
    return fn;
}

static system_freeaddrinfo_fn resolve_system_freeaddrinfo(void) {
    static system_freeaddrinfo_fn fn = NULL;
    if (!fn) {
        fn = (system_freeaddrinfo_fn)dlsym(RTLD_NEXT, "freeaddrinfo");
    }
    if (!fn) {
        fn = (system_freeaddrinfo_fn)dlsym(RTLD_DEFAULT, "freeaddrinfo");
    }
    return fn;
}

static const char *pscalHostsPath(void) {
    const char *root = getenv("PSCALI_CONTAINER_ROOT");
    if (!root || !*root) {
        root = getenv("HOME");
    }
    if (!root || !*root) {
        return NULL;
    }
    static char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/etc/hosts", root);
    if (written < 0 || written >= (int)sizeof(path)) {
        return NULL;
    }
    return path;
}

static void copyFallbackHosts(const char *source, const char *dest_dir, const char *dest_path) {
    if (!source || !dest_dir || !dest_path) return;
    struct stat st;
    if (stat(dest_dir, &st) != 0) {
        mkdir(dest_dir, 0700);
    }
    FILE *in = fopen(source, "r");
    if (!in) return;
    FILE *out = fopen(dest_path, "w");
    if (!out) {
        fclose(in);
        return;
    }
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
}

static FILE *openHostsFile(const char **out_path) {
    const char *primary = pscalHostsPath();
    if (primary) {
        FILE *fp = fopen(primary, "r");
        if (fp) {
            if (out_path) *out_path = primary;
            return fp;
        }
    }
    const char *fallback = "/etc/hosts";
    FILE *fp = fopen(fallback, "r");
    if (fp) {
        if (out_path) *out_path = fallback;
        // Attempt to seed the container copy for next time.
        if (primary) {
            char dir[PATH_MAX];
            strncpy(dir, primary, sizeof(dir));
            dir[sizeof(dir) - 1] = '\0';
            char *slash = strrchr(dir, '/');
            if (slash) {
                *slash = '\0';
                copyFallbackHosts(fallback, dir, primary);
            }
        }
        return fp;
    }
    return NULL;
}

static bool parseServicePort(const char *service, int *out_port) {
    if (!service || !*service) {
        *out_port = 0;
        return true;
    }
    char *end = NULL;
    errno = 0;
    long val = strtol(service, &end, 10);
    if (errno == 0 && end && *end == '\0' && val >= 0 && val <= 65535) {
        *out_port = (int)val;
        return true;
    }
    // Non-numeric services are left to the system resolver.
    return false;
}

static struct addrinfo *makeAddrinfoV4(const struct addrinfo *hints,
                                       const struct in_addr *addr,
                                       int port,
                                       const char *canonname,
                                       bool copy_canon) {
    struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
    struct sockaddr_in *sa = (struct sockaddr_in *)calloc(1, sizeof(struct sockaddr_in));
    if (!ai || !sa) {
        free(ai);
        free(sa);
        return NULL;
    }
    sa->sin_family = AF_INET;
    sa->sin_port = htons((uint16_t)port);
    sa->sin_addr = *addr;
    ai->ai_family = AF_INET;
    ai->ai_socktype = hints ? hints->ai_socktype : 0;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_flags = AI_NUMERICHOST | (hints ? (hints->ai_flags & AI_PASSIVE) : 0);
    ai->ai_addrlen = sizeof(struct sockaddr_in);
    ai->ai_addr = (struct sockaddr *)sa;
    if (copy_canon && (hints && (hints->ai_flags & AI_CANONNAME)) && canonname) {
        ai->ai_canonname = strdup(canonname);
    }
    return ai;
}

static struct addrinfo *makeAddrinfoV6(const struct addrinfo *hints,
                                       const struct in6_addr *addr,
                                       int port,
                                       const char *canonname,
                                       bool copy_canon) {
    struct addrinfo *ai = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
    struct sockaddr_in6 *sa = (struct sockaddr_in6 *)calloc(1, sizeof(struct sockaddr_in6));
    if (!ai || !sa) {
        free(ai);
        free(sa);
        return NULL;
    }
    sa->sin6_family = AF_INET6;
    sa->sin6_port = htons((uint16_t)port);
    sa->sin6_addr = *addr;
    ai->ai_family = AF_INET6;
    ai->ai_socktype = hints ? hints->ai_socktype : 0;
    ai->ai_protocol = hints ? hints->ai_protocol : 0;
    ai->ai_flags = AI_NUMERICHOST | (hints ? (hints->ai_flags & AI_PASSIVE) : 0);
    ai->ai_addrlen = sizeof(struct sockaddr_in6);
    ai->ai_addr = (struct sockaddr *)sa;
    if (copy_canon && (hints && (hints->ai_flags & AI_CANONNAME)) && canonname) {
        ai->ai_canonname = strdup(canonname);
    }
    return ai;
}

static void appendAddrinfo(struct addrinfo **head, struct addrinfo *node) {
    if (!node) return;
    if (!*head) {
        *head = node;
        return;
    }
    struct addrinfo *tail = *head;
    while (tail->ai_next) tail = tail->ai_next;
    tail->ai_next = node;
}

static bool hostsLookup(const char *node, const char *service,
                        const struct addrinfo *hints,
                        struct addrinfo **out_res) {
    const char *path = NULL;
    FILE *fp = openHostsFile(&path);
    if (!fp) {
        return false;
    }
    int port = 0;
    if (!parseServicePort(service, &port)) {
        fclose(fp);
        return false;
    }
    char line[1024];
    struct addrinfo *head = NULL;
    while (fgets(line, sizeof(line), fp)) {
        // Strip comments
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        // Tokenize
        char *saveptr = NULL;
        char *ip = strtok_r(line, " \t\r\n", &saveptr);
        if (!ip) continue;
        bool matched = false;
        char *name = NULL;
        while ((name = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
            if (strcasecmp(name, node) == 0) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        // Build addrinfo for matching entry
        if ((hints == NULL || hints->ai_family == AF_UNSPEC || hints->ai_family == AF_INET)) {
            struct in_addr addr4;
            if (inet_pton(AF_INET, ip, &addr4) == 1) {
                appendAddrinfo(&head, makeAddrinfoV4(hints, &addr4, port, node, head == NULL));
                continue;
            }
        }
        if ((hints == NULL || hints->ai_family == AF_UNSPEC || hints->ai_family == AF_INET6)) {
            struct in6_addr addr6;
            if (inet_pton(AF_INET6, ip, &addr6) == 1) {
                appendAddrinfo(&head, makeAddrinfoV6(hints, &addr6, port, node, head == NULL));
                continue;
            }
        }
    }
    fclose(fp);
    if (!head) {
        return false;
    }
    *out_res = head;
    return true;
}

int pscalHostsGetAddrInfo(const char *node, const char *service,
                          const struct addrinfo *hints, struct addrinfo **res) {
    if (!node) {
        system_getaddrinfo_fn sys = resolve_system_getaddrinfo();
        if (sys) return sys(node, service, hints, res);
        return EAI_FAIL;
    }
    if (hostsLookup(node, service, hints, res)) {
        return 0;
    }
    system_getaddrinfo_fn sys = resolve_system_getaddrinfo();
    if (sys) {
        return sys(node, service, hints, res);
    }
    return EAI_FAIL;
}

void pscalHostsFreeAddrInfo(struct addrinfo *ai) {
    system_freeaddrinfo_fn sys_free = resolve_system_freeaddrinfo();
    if (sys_free) {
        sys_free(ai);
        return;
    }
    // Fallback manual free if dlsym fails
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai->ai_canonname);
        free(ai->ai_addr);
        free(ai);
        ai = next;
    }
}

// Exported shims so any library calls (e.g., OpenSSH) also honour the container hosts file.
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    // Prevent recursion if the system resolver ends up calling back here.
    static __thread int depth = 0;
    if (depth > 0) {
        system_getaddrinfo_fn sys = resolve_system_getaddrinfo();
        if (sys) {
            return sys(node, service, hints, res);
        }
        return EAI_FAIL;
    }
    depth++;
    int rc = pscalHostsGetAddrInfo(node, service, hints, res);
    depth--;
    return rc;
}

void freeaddrinfo(struct addrinfo *ai) {
    // Guard recursion for symmetry with getaddrinfo.
    static __thread int depth = 0;
    if (depth > 0) {
        system_freeaddrinfo_fn sys_free = resolve_system_freeaddrinfo();
        if (sys_free) {
            sys_free(ai);
            return;
        }
    }
    depth++;
    pscalHostsFreeAddrInfo(ai);
    depth--;
}
