#include "common/pscal_hosts.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>

#if defined(PSCAL_TARGET_IOS)
#include <dlfcn.h>
#endif

#ifndef PSCAL_HOSTS_CUSTOM_IMPL
typedef int (*system_getaddrinfo_fn)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
typedef void (*system_freeaddrinfo_fn)(struct addrinfo *);

static void pscalHostsFreeAddrInfoChain(struct addrinfo *ai) {
    while (ai) {
        struct addrinfo *next = ai->ai_next;
        free(ai->ai_canonname);
        free(ai->ai_addr);
        free(ai);
        ai = next;
    }
}
#endif /* !PSCAL_HOSTS_CUSTOM_IMPL */

#if defined(PSCAL_TARGET_IOS)
#ifndef PSCAL_HOSTS_CUSTOM_IMPL
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
#endif /* !PSCAL_HOSTS_CUSTOM_IMPL */
#else
#ifndef PSCAL_HOSTS_CUSTOM_IMPL
static system_getaddrinfo_fn resolve_system_getaddrinfo(void) {
    return &getaddrinfo;
}
static system_freeaddrinfo_fn resolve_system_freeaddrinfo(void) {
    return &freeaddrinfo;
}
#endif /* !PSCAL_HOSTS_CUSTOM_IMPL */
#endif

static bool buildHostsPath(const char *root, char *out, size_t out_size) {
    if (!root || !*root || !out || out_size == 0) {
        return false;
    }
    int written = snprintf(out, out_size, "%s/etc/hosts", root);
    return written > 0 && written < (int)out_size;
}

static const char *pscalHostsPath(void) {
#if defined(PSCAL_TARGET_IOS)
    static char path[PATH_MAX];
    const char *candidates[7] = {0};
    size_t count = 0;
    const char *etc_root = getenv("PSCALI_ETC_ROOT");
    if (etc_root && *etc_root) {
        candidates[count++] = etc_root;
    }
    const char *env_root = getenv("PSCALI_CONTAINER_ROOT");
    if (env_root && *env_root) {
        candidates[count++] = env_root;
    }
    const char *home = getenv("HOME");
    char tmp1[PATH_MAX];
    char tmp2[PATH_MAX];
    if (home && *home) {
        candidates[count++] = home;
        // parent of HOME
        strncpy(tmp1, home, sizeof(tmp1));
        tmp1[sizeof(tmp1) - 1] = '\0';
        char *slash = strrchr(tmp1, '/');
        if (slash && slash != tmp1) {
            *slash = '\0';
            candidates[count++] = tmp1;
            // parent of parent
            strncpy(tmp2, tmp1, sizeof(tmp2));
            tmp2[sizeof(tmp2) - 1] = '\0';
            slash = strrchr(tmp2, '/');
            if (slash && slash != tmp2) {
                *slash = '\0';
                candidates[count++] = tmp2;
            }
        }
    }
    // Last resort: workspace root + /etc
    const char *ws = getenv("PSCALI_WORKSPACE_ROOT");
    if (ws && *ws) {
        candidates[count++] = ws;
    }
    for (size_t i = 0; i < count; ++i) {
        if (buildHostsPath(candidates[i], path, sizeof(path))) {
            struct stat st;
            if (stat(path, &st) == 0) {
                return path;
            }
        }
    }
    return NULL;
#else
    return "/etc/hosts";
#endif
}

#ifndef PSCAL_HOSTS_CUSTOM_IMPL
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
    return false;
}

static struct addrinfo *makeAddrinfoV4(const struct addrinfo *hints,
                                       const struct in_addr *addr,
                                       int port,
                                       const char *canonname) {
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
    if (canonname && hints && (hints->ai_flags & AI_CANONNAME)) {
        ai->ai_canonname = strdup(canonname);
    }
    return ai;
}

static struct addrinfo *makeAddrinfoV6(const struct addrinfo *hints,
                                       const struct in6_addr *addr,
                                       int port,
                                       const char *canonname) {
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
    if (canonname && hints && (hints->ai_flags & AI_CANONNAME)) {
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

static struct addrinfo *cloneAddrinfoChain(const struct addrinfo *src) {
    struct addrinfo *head = NULL;
    const struct addrinfo *it = src;
    while (it) {
        struct addrinfo *copy = (struct addrinfo *)calloc(1, sizeof(struct addrinfo));
        if (!copy) {
            pscalHostsFreeAddrInfoChain(head);
            return NULL;
        }
        memcpy(copy, it, sizeof(struct addrinfo));
        if (it->ai_addr && it->ai_addrlen > 0) {
            struct sockaddr *addr = (struct sockaddr *)malloc(it->ai_addrlen);
            if (!addr) {
                free(copy);
                pscalHostsFreeAddrInfoChain(head);
                return NULL;
            }
            memcpy(addr, it->ai_addr, it->ai_addrlen);
            copy->ai_addr = addr;
            copy->ai_addrlen = it->ai_addrlen;
        }
        if (it->ai_canonname) {
            copy->ai_canonname = strdup(it->ai_canonname);
        }
        copy->ai_next = NULL;
        appendAddrinfo(&head, copy);
        it = it->ai_next;
    }
    return head;
}
#endif /* !PSCAL_HOSTS_CUSTOM_IMPL */

const char *pscalHostsGetContainerPath(void) {
#if defined(PSCAL_TARGET_IOS)
    return pscalHostsPath();
#else
    return "/etc/hosts";
#endif
}

static int g_pscal_hosts_log_override = -1; /* -1 auto, 0 off, 1 on */

bool pscalHostsLogEnabled(void) {
#if defined(PSCAL_TARGET_IOS)
    if (g_pscal_hosts_log_override != -1) {
        return g_pscal_hosts_log_override != 0;
    }
    const char *val = getenv("PSCALI_DEBUG_HOSTS");
    return val && *val;
#else
    return false;
#endif
}

void pscalHostsSetLogEnabled(int enabled) {
#if defined(PSCAL_TARGET_IOS)
    g_pscal_hosts_log_override = enabled;
#else
    (void)enabled;
#endif
}

#ifndef PSCAL_HOSTS_CUSTOM_IMPL
static bool readHostsFile(const char *path, const char *node, int port,
                          const struct addrinfo *hints, struct addrinfo **head) {
    if (!path || !head) return false;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (pscalHostsLogEnabled()) {
            fprintf(stderr, "pscal_hosts: unable to open hosts file '%s': %s\n", path, strerror(errno));
        }
        return false;
    }
    if (pscalHostsLogEnabled()) {
        fprintf(stderr, "pscal_hosts: consulting hosts file '%s'\n", path);
    }
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
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
        if (!hints || hints->ai_family == AF_UNSPEC || hints->ai_family == AF_INET) {
            struct in_addr addr4;
            if (inet_pton(AF_INET, ip, &addr4) == 1) {
                appendAddrinfo(head, makeAddrinfoV4(hints, &addr4, port, node));
                continue;
            }
        }
        if (!hints || hints->ai_family == AF_UNSPEC || hints->ai_family == AF_INET6) {
            struct in6_addr addr6;
            if (inet_pton(AF_INET6, ip, &addr6) == 1) {
                appendAddrinfo(head, makeAddrinfoV6(hints, &addr6, port, node));
                continue;
            }
        }
    }
    fclose(fp);
    return true;
}

static bool hostsLookup(const char *node, const char *service,
                        const struct addrinfo *hints,
                        struct addrinfo **out_res) {
    int port = 0;
    if (!parseServicePort(service, &port)) {
        return false;
    }

    const char *container_path = pscalHostsPath();
    const char *fallback_path = "/etc/hosts";
    struct addrinfo *head = NULL;

    if (container_path) {
        readHostsFile(container_path, node, port, hints, &head);
    }
    if (fallback_path && (!container_path || strcmp(fallback_path, container_path) != 0)) {
        readHostsFile(fallback_path, node, port, hints, &head);
    }

    if (!head) {
        if (pscalHostsLogEnabled()) {
            fprintf(stderr, "pscal_hosts: no hosts entry found for '%s' in %s%s%s\n",
                    node,
                    container_path ? container_path : "(no container path)",
                    (container_path && fallback_path && strcmp(container_path, fallback_path) != 0) ? " or " : "",
                    (container_path && fallback_path && strcmp(container_path, fallback_path) != 0) ? fallback_path : "");
        }
        return false;
    }
    *out_res = head;
    return true;
}
#endif /* !PSCAL_HOSTS_CUSTOM_IMPL */

#ifndef PSCAL_HOSTS_CUSTOM_IMPL
int pscalHostsGetAddrInfo(const char *node, const char *service,
                          const struct addrinfo *hints, struct addrinfo **res) {
    if (!node) {
        system_getaddrinfo_fn sys = resolve_system_getaddrinfo();
        if (sys) return sys(node, service, hints, res);
        return EAI_FAIL;
    }

    // Prefer explicit hosts file mapping when service is numeric (or empty).
    if (hostsLookup(node, service, hints, res)) {
        return 0;
    }

    system_getaddrinfo_fn sys = resolve_system_getaddrinfo();
    system_freeaddrinfo_fn sys_free = resolve_system_freeaddrinfo();
    if (!sys || !sys_free) {
        return EAI_FAIL;
    }

    struct addrinfo *tmp = NULL;
    int rc = sys(node, service, hints, &tmp);
    if (rc != 0) {
        return rc;
    }
    // Clone into our own allocations so callers can free with pscalHostsFreeAddrInfo.
    struct addrinfo *cloned = cloneAddrinfoChain(tmp);
    sys_free(tmp);
    if (!cloned) {
        return EAI_MEMORY;
    }
    *res = cloned;
    return 0;
}

void pscalHostsFreeAddrInfo(struct addrinfo *ai) {
    pscalHostsFreeAddrInfoChain(ai);
}
#endif /* PSCAL_HOSTS_CUSTOM_IMPL */
