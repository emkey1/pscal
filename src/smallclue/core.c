/*
 * NOTE: For iPadOS/iOS, every new smallclue applet must also be wired into
 * src/smallclue/integration.c and src/shell/builtins.c so exsh can invoke it.
 * See Docs/notes_smallclu_ios.md for the full checklist before landing changes.
 */
#include "smallclue/smallclue.h"

#include "common/runtime_tty.h"
#include "smallclue/nextvi_app.h"
#include "smallclue/openssh_app.h"
#include "common/runtime_clipboard.h"
#if defined(PSCAL_HAS_LIBCURL)
#include <curl/curl.h>
#endif
#if defined(PSCAL_TARGET_IOS)
#include "common/path_virtualization.h"
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <libgen.h>
#include <pwd.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/select.h>
#include <glob.h>
#include "common/pscal_hosts.h"
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include "common/path_truncate.h"
#include "common/path_virtualization.h"
void pscalRuntimeDebugLog(const char *message) __attribute__((weak));
#endif
#if defined(PSCAL_TARGET_IOS)
__attribute__((weak_import)) void PSCALRuntimeUpdateWindowSize(int columns, int rows);
__attribute__((weak)) void PSCALRuntimeUpdateWindowSize(int columns, int rows) { (void)columns; (void)rows; }
#endif
#include <termios.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/mount.h>
#define SMALLCLUE_HAVE_STATFS 1
#else
#include <sys/statvfs.h>
#define SMALLCLUE_HAVE_STATVFS 1
#endif
#if SMALLCLUE_HAS_IFADDRS
#include <ifaddrs.h>
#include <net/if.h>
#endif

static const char *smallclueResolvePath(const char *path, char *buffer, size_t buflen) {
    if (!path) {
        return NULL;
    }
#if defined(PSCAL_TARGET_IOS)
    if (pathTruncateExpand(path, buffer, buflen)) {
        return buffer;
    }
#endif
    (void)buffer;
    (void)buflen;
    return path;
}

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#define SMALLCLUE_GETOPT_NEEDS_OPTRESET 1
#else
#define SMALLCLUE_GETOPT_NEEDS_OPTRESET 0
#endif

#if SMALLCLUE_GETOPT_NEEDS_OPTRESET
extern int optreset;
#endif

#if defined(__APPLE__)
extern bool shellRuntimeConsumeExitRequested(void) __attribute__((weak_import));
#elif defined(__GNUC__)
extern bool shellRuntimeConsumeExitRequested(void) __attribute__((weak));
#else
bool shellRuntimeConsumeExitRequested(void);
#endif

extern bool pscalRuntimeConsumeSigint(void);
#if defined(PSCAL_TARGET_IOS)
#if defined(__APPLE__)
extern char *pscalRuntimeCopySessionLog(void) __attribute__((weak_import));
extern void pscalRuntimeResetSessionLog(void) __attribute__((weak_import));
#elif defined(__GNUC__)
extern char *pscalRuntimeCopySessionLog(void) __attribute__((weak));
extern void pscalRuntimeResetSessionLog(void) __attribute__((weak));
#else
char *pscalRuntimeCopySessionLog(void);
void pscalRuntimeResetSessionLog(void);
#endif

// Provide weak fallbacks so smallclue can link when the Swift bridge is absent.
__attribute__((weak)) char *pscalRuntimeCopySessionLog(void) { return NULL; }
__attribute__((weak)) void pscalRuntimeResetSessionLog(void) { }
#endif

__attribute__((weak)) bool shellRuntimeConsumeExitRequested(void) {
    return false;
}

static void smallclueClearPendingSignals(void) {
    sigset_t watchset;
    sigemptyset(&watchset);
    sigaddset(&watchset, SIGINT);
    sigaddset(&watchset, SIGTSTP);
    sigset_t oldset;
    if (sigprocmask(SIG_BLOCK, &watchset, &oldset) != 0) {
        return;
    }
    sigset_t pending;
    while (sigpending(&pending) == 0 &&
           (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTSTP))) {
        int signo = 0;
        if (sigwait(&watchset, &signo) != 0) {
            break;
        }
        (void)signo;
    }
    sigprocmask(SIG_SETMASK, &oldset, NULL);
}

static bool smallclueShouldAbort(int *out_status) {
    if (pscalRuntimeConsumeSigint()) {
        if (out_status) {
            *out_status = 130;
        }
        return true;
    }

    if (shellRuntimeConsumeExitRequested) {
        if (shellRuntimeConsumeExitRequested()) {
            if (out_status) {
                *out_status = 130;
            }
            return true;
        }
    }

    sigset_t watchset;
    sigemptyset(&watchset);
    sigaddset(&watchset, SIGINT);
    sigaddset(&watchset, SIGTSTP);
    sigset_t oldset;
    if (sigprocmask(SIG_BLOCK, &watchset, &oldset) == 0) {
        sigset_t pending;
        if (sigpending(&pending) == 0 &&
            (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTSTP))) {
            int signo = 0;
            if (sigwait(&watchset, &signo) == 0) {
                if (out_status) {
                    *out_status = (signo == SIGINT) ? 130 : 148;
                }
                sigprocmask(SIG_SETMASK, &oldset, NULL);
                return true;
            }
        }
        sigprocmask(SIG_SETMASK, &oldset, NULL);
    }
    return false;
}

static void smallclueResetGetopt(void) {
    optind = 1;
#if SMALLCLUE_GETOPT_NEEDS_OPTRESET
    optreset = 1;
#endif
}

static void smallclueEnvClearAll(void) {
    extern char **environ;
    if (!environ) {
        return;
    }
    size_t count = 0;
    for (char **envp = environ; *envp; ++envp) {
        count++;
    }
    if (count == 0) {
        return;
    }
    char **names = (char **)calloc(count, sizeof(char *));
    if (!names) {
        return;
    }
    size_t idx = 0;
    for (char **envp = environ; *envp; ++envp) {
        char *eq = strchr(*envp, '=');
        if (!eq) {
            continue;
        }
        size_t len = (size_t)(eq - *envp);
        char *name = (char *)malloc(len + 1);
        if (!name) {
            continue;
        }
        memcpy(name, *envp, len);
        name[len] = '\0';
        names[idx++] = name;
    }
    for (size_t i = 0; i < idx; ++i) {
        if (names[i]) {
            unsetenv(names[i]);
            free(names[i]);
        }
    }
    free(names);
}

enum {
    PAGER_KEY_ARROW_UP = 1000,
    PAGER_KEY_ARROW_DOWN,
    PAGER_KEY_PAGE_UP,
    PAGER_KEY_PAGE_DOWN
};

typedef struct {
    char **lines;
    size_t count;
    size_t capacity;
} PagerBuffer;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} SmallclueLineVector;

static int smallclueEchoCommand(int argc, char **argv);
static int smallclueLsCommand(int argc, char **argv);
static int smallclueCatCommand(int argc, char **argv);
static int smallcluePagerCommand(int argc, char **argv);
static int smallclueClearCommand(int argc, char **argv);
static int smallclueRmCommand(int argc, char **argv);
static int smallclueCpCommand(int argc, char **argv);
static int smallclueMvCommand(int argc, char **argv);
static int smallcluePwdCommand(int argc, char **argv);
static int smallclueEnvCommand(int argc, char **argv);
static int smallclueChmodCommand(int argc, char **argv);
static int smallclueDateCommand(int argc, char **argv);
static int smallclueCalCommand(int argc, char **argv);
static int smallclueHeadCommand(int argc, char **argv);
static int smallclueGrepCommand(int argc, char **argv);
static int smallclueWcCommand(int argc, char **argv);
static int smallclueDuCommand(int argc, char **argv);
static int smallclueFindCommand(int argc, char **argv);
static int smallclueTailCommand(int argc, char **argv);
static int smallclueTouchCommand(int argc, char **argv);
static int smallclueSttyCommand(int argc, char **argv);
static int smallclueResizeCommand(int argc, char **argv);
static int smallclueSortCommand(int argc, char **argv);
static int smallclueUniqCommand(int argc, char **argv);
static int smallclueSedCommand(int argc, char **argv);
static int smallclueCutCommand(int argc, char **argv);
static int smallclueTrCommand(int argc, char **argv);
static int smallclueIdCommand(int argc, char **argv);
static void smallclueDfFormatSize(char *buf, size_t bufsize, unsigned long long bytes, bool human);
static int smallclueTrueCommand(int argc, char **argv);
static int smallclueFalseCommand(int argc, char **argv);
static int smallclueSleepCommand(int argc, char **argv);
static int smallclueWatchCommand(int argc, char **argv);
static int smallclueBasenameCommand(int argc, char **argv);
static int smallclueDirnameCommand(int argc, char **argv);
static int smallclueTeeCommand(int argc, char **argv);
static int smallclueTestCommand(int argc, char **argv);
static int smallclueBracketCommand(int argc, char **argv);
static int smallclueXargsCommand(int argc, char **argv);
static int smallcluePsCommand(int argc, char **argv);
static int smallclueKillCommand(int argc, char **argv);
static int smallclueMkdirCommand(int argc, char **argv);
static int smallclueRmdirCommand(int argc, char **argv);
static int smallclueLnCommand(int argc, char **argv);
static int smallclueTypeCommand(int argc, char **argv);
static int smallclueFileCommand(int argc, char **argv);
static int __attribute__((unused)) smallclueLicensesCommand(int argc, char **argv);
static const char *smallclueLeafName(const char *path);
static int smallclueBuildPath(char *buf, size_t buf_size, const char *dir, const char *leaf);
static void smallclueTrimTrailingSlashes(char *path);
static bool smallclueChopParentDirectory(char *path);
static int smallclueRemovePathWithLabel(const char *label, const char *path, bool recursive, bool force);
static int smallclueCopyFile(const char *label, const char *src, const char *dst);
static int smallclueMkdirParents(const char *path, mode_t mode);
static void smallclueGetTerminalSize(int *rows, int *cols);
static int smallclueElvisCommand(int argc, char **argv);
static int smallclueSshCommand(int argc, char **argv);
static int smallclueScpCommand(int argc, char **argv);
static int smallclueSftpCommand(int argc, char **argv);
static int smallclueSshKeygenCommand(int argc, char **argv);
static int smallcluePbcopyCommand(int argc, char **argv);
static int smallcluePbpasteCommand(int argc, char **argv);
#if defined(SMALLCLUE_WITH_EXSH)
extern int exsh_main(int argc, char **argv);
static int smallclueShCommand(int argc, char **argv);
#endif
static int smallclueUptimeCommand(int argc, char **argv);
static int smallcluePingCommand(int argc, char **argv);
static int smallclueMarkdownCommand(int argc, char **argv);
static int smallclueCurlCommand(int argc, char **argv);
static int smallclueWgetCommand(int argc, char **argv);
static int smallclueTelnetCommand(int argc, char **argv);
static int smallclueTracerouteCommand(int argc, char **argv);
static int smallclueNslookupCommand(int argc, char **argv);
static int smallclueHostCommand(int argc, char **argv);
#if SMALLCLUE_HAS_IFADDRS
static int smallclueIpAddrCommand(int argc, char **argv);
#endif
static int smallclueDfCommand(int argc, char **argv);
#if defined(PSCAL_TARGET_IOS)
static int smallclueDmesgCommand(int argc, char **argv);
static int smallclueHelpCommand(int argc, char **argv);
#endif

static int smallclueNslookupCommand(int argc, char **argv) {
    const char *usage = "usage: nslookup [-v] host [port]\n";
    bool verbose = false;
    smallclueResetGetopt();
    int opt;
    while ((opt = getopt(argc, argv, "v")) != -1) {
        switch (opt) {
            case 'v':
                verbose = true;
                break;
            default:
            fputs(usage, stderr);
            return 1;
        }
    }
    if (verbose) {
        pscalHostsSetLogEnabled(1);
    } else {
        pscalHostsSetLogEnabled(-1);
    }
    if (optind >= argc) {
        fputs(usage, stderr);
        return 1;
    }
    const char *host = argv[optind];
    const char *service = (optind + 1 < argc) ? argv[optind + 1] : "53";

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_family = AF_UNSPEC;
    struct addrinfo *res = NULL;
    int gai = pscalHostsGetAddrInfo(host, service, &hints, &res);
    if (gai != 0 || !res) {
#if defined(PSCAL_TARGET_IOS)
        fprintf(stderr, "nslookup: hosts lookup paths: %s first, then /etc/hosts\n",
                getenv("PSCALI_CONTAINER_ROOT") ? pscalHostsGetContainerPath() : "(no PSCALI_CONTAINER_ROOT)");
#endif
        fprintf(stderr, "nslookup: %s: %s\n", host, gai_strerror(gai));
        return 1;
    }
    printf("Non-authoritative answer for %s:\n", host);
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        char hostbuf[NI_MAXHOST];
        char servbuf[NI_MAXSERV];
        if (getnameinfo(ai->ai_addr, (socklen_t)ai->ai_addrlen,
                        hostbuf, sizeof(hostbuf),
                        servbuf, sizeof(servbuf),
                        NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
            printf("  %s (port %s) family %d\n", hostbuf, servbuf, ai->ai_family);
        }
    }
    pscalHostsFreeAddrInfo(res);
    return 0;
}

static int smallclueHostCommand(int argc, char **argv) {
    const char *usage = "usage: host [-4|-6] [-v] [-t TYPE] host [server]\n";
    int family = AF_UNSPEC;
    bool verbose = false;
    smallclueResetGetopt();
    int opt;
    while ((opt = getopt(argc, argv, "46vt:")) != -1) {
        switch (opt) {
            case '4': family = AF_INET; break;
            case '6': family = AF_INET6; break;
            case 'v': verbose = true; break;
            case 't':
                if (strcasecmp(optarg, "A") == 0) family = AF_INET;
                else if (strcasecmp(optarg, "AAAA") == 0) family = AF_INET6;
                break;
            default:
                fputs(usage, stderr);
                return 1;
        }
    }
    if (verbose) {
        pscalHostsSetLogEnabled(1);
    } else {
        pscalHostsSetLogEnabled(-1);
    }
    if (optind >= argc) {
        fputs(usage, stderr);
        return 1;
    }
    const char *host = argv[optind];
    if (optind + 1 < argc) {
        fprintf(stderr, "host: server override not supported; ignoring '%s'\n", argv[optind + 1]);
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int gai = pscalHostsGetAddrInfo(host, NULL, &hints, &res);
    if (gai != 0 || !res) {
        fprintf(stderr, "%s not found: %s\n", host, gai_strerror(gai));
        return 1;
    }
    bool printed = false;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (family == AF_INET && ai->ai_family != AF_INET) continue;
        if (family == AF_INET6 && ai->ai_family != AF_INET6) continue;
        char addrbuf[NI_MAXHOST];
        if (getnameinfo(ai->ai_addr, (socklen_t)ai->ai_addrlen,
                        addrbuf, sizeof(addrbuf),
                        NULL, 0, NI_NUMERICHOST) == 0) {
            if (ai->ai_family == AF_INET6) {
                printf("%s has IPv6 address %s\n", host, addrbuf);
            } else {
                printf("%s has address %s\n", host, addrbuf);
            }
            printed = true;
        }
    }
    pscalHostsFreeAddrInfo(res);
    if (!printed) {
        fprintf(stderr, "No records found for %s\n", host);
        return 1;
    }
    return 0;
}

typedef struct SmallclueAppletHelp {
    const char *name;
    const char *usage;
} SmallclueAppletHelp;

static const SmallclueApplet kSmallclueApplets[] = {
    {"[", smallclueBracketCommand, "Evaluate expressions"},
    {"basename", smallclueBasenameCommand, "Strip directory prefix"},
    {"cal", smallclueCalCommand, "Show a simple calendar"},
    {"cat", smallclueCatCommand, "Concatenate files"},
    {"chmod", smallclueChmodCommand, "Change file permissions"},
    {"clear", smallclueClearCommand, "Clear the terminal"},
    {"cls", smallclueClearCommand, "Clear the terminal"},
    {"cp", smallclueCpCommand, "Copy files"},
    {"curl", smallclueCurlCommand, "Transfer data from URLs"},
    {"cut", smallclueCutCommand, "Extract fields from lines"},
    {"date", smallclueDateCommand, "Display current date/time"},
    {"dirname", smallclueDirnameCommand, "Strip last path component"},
    {"du", smallclueDuCommand, "Summarize disk usage"},
    {"echo", smallclueEchoCommand, "Print arguments"},
    {"nextvi", smallclueElvisCommand, "Nextvi text editor"},
    {"env", smallclueEnvCommand, "Display or update environment"},
    {"false", smallclueFalseCommand, "Do nothing, unsuccessfully"},
    {"file", smallclueFileCommand, "Identify file types"},
    {"find", smallclueFindCommand, "Search for files"},
    {"grep", smallclueGrepCommand, "Search for patterns"},
    {"head", smallclueHeadCommand, "Print the first lines of files"},
    {"id", smallclueIdCommand, "Print user identity information"},
#if SMALLCLUE_HAS_IFADDRS
    {"ipaddr", smallclueIpAddrCommand, "Show interface IP addresses"},
#endif
    {"host", smallclueHostCommand, "DNS lookup utility"},
    {"kill", smallclueKillCommand, "Send signals to processes"},
    {"less", smallcluePagerCommand, "Paginate file contents"},
    {"ln", smallclueLnCommand, "Create links"},
    {"ls", smallclueLsCommand, "List directory contents"},
    {"md", smallclueMarkdownCommand, "Read Markdown documents"},
    {"mkdir", smallclueMkdirCommand, "Create directories"},
    {"more", smallcluePagerCommand, "Paginate file contents"},
    {"mv", smallclueMvCommand, "Move or rename files"},
    {"nslookup", smallclueNslookupCommand, "DNS lookup utility"},
    {"pbcopy", smallcluePbcopyCommand, "Copy stdin to the system clipboard"},
    {"pbpaste", smallcluePbpasteCommand, "Paste the system clipboard to stdout"},
    {"ping", smallcluePingCommand, "TCP ping utility"},
    {"ps", smallcluePsCommand, "Show simple process information"},
    {"pwd", smallcluePwdCommand, "Print working directory"},
    {"resize", smallclueResizeCommand, "Synchronize terminal rows/columns"},
    {"rm", smallclueRmCommand, "Remove files"},
    {"rmdir", smallclueRmdirCommand, "Remove empty directories"},
    {"sed", smallclueSedCommand, "Stream editor for simple substitutions"},
    {"sleep", smallclueSleepCommand, "Delay for a number of seconds"},
    {"sort", smallclueSortCommand, "Sort lines of text"},
    {"stty", smallclueSttyCommand, "Adjust terminal rows/columns"},
#if defined(SMALLCLUE_WITH_EXSH)
    {"sh", smallclueShCommand, "Run the PSCAL shell front end"},
#endif
    {"scp", smallclueScpCommand, "Securely copy files over SSH"},
    {"sftp", smallclueSftpCommand, "Interactive SFTP client"},
    {"ssh", smallclueSshCommand, "OpenSSH client"},
    {"ssh-keygen", smallclueSshKeygenCommand, "Generate SSH key pairs"},
    {"tail", smallclueTailCommand, "Print the last lines of files"},
    {"tee", smallclueTeeCommand, "Copy stdin to files and stdout"},
    {"telnet", smallclueTelnetCommand, "Simple TCP telnet client"},
    {"test", smallclueTestCommand, "Evaluate expressions"},
    {"touch", smallclueTouchCommand, "Update file timestamps"},
    {"traceroute", smallclueTracerouteCommand, "Trace network path to a host"},
    {"tr", smallclueTrCommand, "Translate or delete characters"},
    {"true", smallclueTrueCommand, "Do nothing, successfully"},
    {"type", smallclueTypeCommand, "Describe command names"},
    {"uniq", smallclueUniqCommand, "Report or omit repeated lines"},
    {"uptime", smallclueUptimeCommand, "Show system uptime"},
    {"watch", smallclueWatchCommand, "Periodically run a command"},
    {"vi", smallclueElvisCommand, "Alias for Nextvi text editor"},
    {"wc", smallclueWcCommand, "Count lines/words/bytes"},
    {"wget", smallclueWgetCommand, "Download files via HTTP(S)"},
    {"xargs", smallclueXargsCommand, "Build command lines from stdin"},
    {"df", smallclueDfCommand, "Report filesystem usage"},
#if defined(PSCAL_TARGET_IOS)
    {"smallclue-help", smallclueHelpCommand, "List available smallclue applets"},
    {"licenses", smallclueLicensesCommand, "View third-party licenses"},
    {"dmesg", smallclueDmesgCommand, "Show PSCAL runtime log for this session"},
#endif
};

static const SmallclueAppletHelp kSmallclueAppletHelp[] = {
    {"[", "[ expression ]\n"
          "  Alias for test; see 'test' for operators"},
    {"basename", "basename PATH [SUFFIX]\n"
                 "  Strip directory prefix and optional suffix"},
    {"cal", "cal [month] [year]\n"
            "  Show a simple calendar"},
    {"cat", "cat [FILE ...]\n"
            "  Concatenate files to stdout"},
    {"chmod", "chmod MODE FILE ...\n"
              "  MODE forms: u+rwx,g-w,o=r, a-wx, 755, 0644"},
    {"clear", "clear\n"
              "  Clear the terminal"},
    {"cls", "cls\n"
            "  Clear the terminal (alias)"},
    {"cp", "cp [-r] SRC... DEST\n"
           "  -r  recursive copy"},
    {"curl", "curl [options] URL...\n"
             "  Common: -o FILE,\n"
             "  -O (remote name)\n"
             "  -L (follow)\n"
             "  -d DATA\n"
             "  -H HEADER"},
    {"cut", "cut -d DELIM -f LIST [FILE...]\n"
            "  -d delimiter (default tab)\n"
            "  -f fields (e.g. 1,3-5)"},
    {"date", "date [+FORMAT]\n"
             "  Show date/time"},
    {"dirname", "dirname PATH\n"
                "  Strip last path component"},
    {"du", "du [-h] [PATH...]\n"
           "  -h human-readable sizes"},
    {"echo", "echo [args...]\n"
             "  Print arguments"},
    {"env", "env [-i] [NAME=VALUE ...] [command]\n"
            "  -i start with empty environment"},
    {"false", "false\n"
              "  Exit with status 1"},
    {"file", "file FILE...\n"
             "  Identify file types"},
    {"find", "find PATH... [expression]\n"
             "  Common: -name PATTERN -type f|d"},
    {"grep", "grep [-i] [-n] [-v] PATTERN [FILE...]\n"
             "  -i ignore case\n"
             "  -n line numbers\n"
             "  -v invert match"},
    {"head", "head [-n N] [FILE...]\n"
             "  Default N=10"},
    {"id", "id\n"
           "  Show uid/gid info"},
#if SMALLCLUE_HAS_IFADDRS
    {"ipaddr", "ipaddr\n"
               "  Show interface IP addresses"},
#endif
    {"kill", "kill [-SIGNAL] PID...\n"
             "  Signals: HUP INT TERM KILL etc."},
    {"less", "less [FILE...]\n"
             "  Pager; navigation: j/k, /, n, g/G, q"},
    {"ln", "ln [-s] TARGET LINK\n"
           "  -s symbolic link"},
    {"ls", "ls [-a] [-l] [-t] [-h] [-d] [--color[=auto|always|never]] [path ...]\n"
           "  -a show entries starting with '.'\n"
           "  -l long format with permissions, ownership, size, time\n"
           "  -t sort by modification time\n"
           "  -h human-readable sizes (with -l)\n"
           "  -d list directories themselves, not their contents"},
    {"md", "md [-i] [FILE]\n"
           "  View Markdown document\n"
           "  -i interactive mode.  Makes ~/Docs browsable"},
    {"mkdir", "mkdir [-p] DIR...\n"
              "  -p create parents as needed"},
    {"more", "more [FILE...]\n"
             "  Pager (alias of less)"},
    {"mv", "mv SRC... DEST\n"
           "  Move or rename files"},
    {"nslookup", "nslookup [-v] host [port]\n"
                 "  DNS lookup utility (UDP port defaults to 53).\n"
                 "  -v prints hosts lookup debugging."},
    {"nextvi", "nextvi [FILE]\n"
               "  Full-screen text editor"},
    {"host", "host [-4|-6] [-v] [-t TYPE] host [server]\n"
             "  -4 IPv4 only\n"
             "  -6 IPv6 only\n"
             "  -t A|AAAA select record type\n"
             "  -v verbose (hosts debug)\n"
             "Server override is ignored."},
    {"pbcopy", "pbcopy\n"
               "  Copy stdin to system clipboard"},
    {"pbpaste", "pbpaste\n"
                "  Paste system clipboard to stdout"},
    {"ping", "ping HOST [PORT]\n"
             "  TCP ping (default port 80)"},
    {"ps", "ps\n"
           "  Show simple process list"},
    {"pwd", "pwd\n"
            "  Print working directory"},
    {"resize", "resize [COLUMNS ROWS]\n"
               "  Report or set terminal size"},
    {"rm", "rm [-r] [-f] FILE...\n"
           "  -r recursive\n"
           "  -f force"},
    {"rmdir", "rmdir DIR...\n"
              "  Remove empty directories"},
    {"sed", "sed 's/old/new/g' [FILE...]\n"
            "  Simple substitution support"},
    {"sleep", "sleep SECONDS\n"
              "  Pause execution"},
    {"sort", "sort [-r] [-n]\n"
             "  -r reverse\n"
             "  -n numeric"},
    {"stty", "stty [-a] [rows N] [cols N]\n"
             "  Adjust/report terminal size"},
#if defined(SMALLCLUE_WITH_EXSH)
    {"sh", "sh\n"
           "  Launch PSCAL shell front end"},
#endif
    {"scp", "scp [-P PORT] SRC... DEST\n"
            "  Uses OpenSSH scp"},
    {"sftp", "sftp [-P PORT] [USER@]HOST\n"
             "  Interactive SFTP client"},
    {"ssh", "ssh [-p PORT] [USER@]HOST [command]\n"
            "  OpenSSH client"},
    {"ssh-keygen", "ssh-keygen [-t TYPE] [-f FILE] [-C COMMENT]\n"
                   "  Generate SSH key pairs"},
    {"tail", "tail [-n N] [FILE...]\n"
             "  Default N=10"},
    {"tee", "tee [-a] FILE...\n"
            "  -a append"},
    {"telnet", "telnet [-p PORT] HOST\n"
               "  Connect to HOST over TCP (default port 23) and relay stdin/stdout"},
    {"traceroute", "traceroute HOST [PORT]\n"
                   "  Trace network path using the system traceroute command"},
    {"test", "test EXPRESSION\n"
             "  File: -f -d -e; String: = != -z; Int: -eq -ne -lt -le -gt -ge"},
    {"touch", "touch FILE...\n"
              "  Update timestamps or create empty file"},
    {"tr", "tr SET1 SET2\n"
           "  Translate/delete characters"},
    {"true", "true\n"
             "  Exit status 0"},
    {"type", "type NAME...\n"
             "  Describe how a name is resolved"},
    {"uniq", "uniq [-c] [-d] [-u] [FILE]\n"
             "  -c count\n"
             "  -d duplicates only\n"
             "  -u unique only"},
    {"uptime", "uptime\n"
               "  Show system uptime"},
    {"vi", "vi [FILE]\n"
           "  Alias for nextvi"},
    {"watch", "watch [-n SECONDS] command...\n"
              "  Periodically run a command"},
    {"wc", "wc [-l] [-w] [-c] [FILE...]\n"
           "  Count lines/words/bytes"},
    {"wget", "wget [options] URL...\n"
             "  Common: -O FILE\n"
             "  --header\n"
             "  --post-data\n"
             "  -q\n"
             "  -nv"},
    {"xargs", "xargs [-n N] [-0]\n"
              "  Build command lines from stdin"},
    {"df", "df [-h]\n"
           "  -h human-readable sizes"},
    {"nslookup", "nslookup [-v] host [port]\n"
                 "  DNS lookup utility (UDP port defaults to 53). -v prints hosts lookup debugging."},
#if defined(PSCAL_TARGET_IOS)
    {"smallclue-help", "smallclue-help [command]\n"
                       "  Without arguments: list all applets\n"
                       "  With a command: show usage if available"},
    {"licenses", "licenses\n"
                 "  Browse PSCAL and third-party licenses; use arrows/enter to view"},
    {"dmesg", "dmesg\n"
              "  Show PSCAL runtime log for this session"},
#endif
    {NULL, NULL}
};

static size_t kSmallclueAppletCount = sizeof(kSmallclueApplets) / sizeof(kSmallclueApplets[0]);

static const char * __attribute__((unused)) smallclueLookupHelp(const char *name) {
    for (const SmallclueAppletHelp *h = kSmallclueAppletHelp; h && h->name; ++h) {
        if (strcmp(h->name, name) == 0) {
            return h->usage;
        }
    }
    return NULL;
}

static const char *pager_command_name(const char *name);
static int pager_read_key(void);

static void pagerBell(void) {
    fputc('\a', stdout);
    fflush(stdout);
}

static bool pagerBufferAppendLine(PagerBuffer *buffer, char *line) {
    if (!buffer) {
        return false;
    }
    if (buffer->count == buffer->capacity) {
        size_t new_capacity = buffer->capacity ? buffer->capacity * 2 : 64;
        char **new_lines = (char **)realloc(buffer->lines, new_capacity * sizeof(char *));
        if (!new_lines) {
            return false;
        }
        buffer->lines = new_lines;
        buffer->capacity = new_capacity;
    }
    buffer->lines[buffer->count++] = line;
    return true;
}

static void pagerBufferFree(PagerBuffer *buffer) {
    if (!buffer) {
        return;
    }
    for (size_t i = 0; i < buffer->count; ++i) {
        free(buffer->lines[i]);
    }
    free(buffer->lines);
    buffer->lines = NULL;
    buffer->count = 0;
    buffer->capacity = 0;
}

static bool smallclueLineVectorAppend(SmallclueLineVector *vec, const char *data, size_t len) {
    if (!vec || !data) {
        return false;
    }
    if (vec->count == vec->capacity) {
        size_t newcap = vec->capacity ? vec->capacity * 2 : 64;
        char **ptr = (char **)realloc(vec->items, newcap * sizeof(char *));
        if (!ptr) {
            return false;
        }
        vec->items = ptr;
        vec->capacity = newcap;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, data, len);
    copy[len] = '\0';
    vec->items[vec->count++] = copy;
    return true;
}

static void smallclueLineVectorFree(SmallclueLineVector *vec) {
    if (!vec) {
        return;
    }
    for (size_t i = 0; i < vec->count; ++i) {
        free(vec->items[i]);
    }
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

static bool smallclueReadTokensFromStdin(SmallclueLineVector *vec) {
    char *token = NULL;
    size_t tokcap = 0;
    size_t toklen = 0;
    int ch;
    while ((ch = fgetc(stdin)) != EOF) {
        if (isspace((unsigned char)ch)) {
            if (toklen > 0) {
                if (!smallclueLineVectorAppend(vec, token, toklen)) {
                    free(token);
                    return false;
                }
                toklen = 0;
            }
            continue;
        }
        if (toklen + 1 >= tokcap) {
            size_t newcap = tokcap ? tokcap * 2 : 64;
            char *tmp = (char *)realloc(token, newcap);
            if (!tmp) {
                free(token);
                return false;
            }
            token = tmp;
            tokcap = newcap;
        }
        token[toklen++] = (char)ch;
    }
    if (toklen > 0) {
        bool ok = smallclueLineVectorAppend(vec, token, toklen);
        free(token);
        return ok;
    }
    free(token);
    return true;
}

static int smallcluePsCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    pid_t pid = getpid();
    pid_t ppid = getppid();
    uid_t uid = getuid();
    const char *cmd = argv && argv[0] ? argv[0] : "ps";
    printf(" PID   PPID   UID COMMAND\n");
    printf("%4d %6d %5d %s\n", (int)pid, (int)ppid, (int)uid, cmd);
    return 0;
}

typedef struct {
    const char *name;
    int value;
} SmallclueSignalName;

static const SmallclueSignalName kSignalNames[] = {
#ifdef SIGHUP
    {"HUP", SIGHUP},
#endif
#ifdef SIGINT
    {"INT", SIGINT},
#endif
#ifdef SIGQUIT
    {"QUIT", SIGQUIT},
#endif
#ifdef SIGILL
    {"ILL", SIGILL},
#endif
#ifdef SIGABRT
    {"ABRT", SIGABRT},
#endif
#ifdef SIGKILL
    {"KILL", SIGKILL},
#endif
#ifdef SIGALRM
    {"ALRM", SIGALRM},
#endif
#ifdef SIGTERM
    {"TERM", SIGTERM},
#endif
#ifdef SIGUSR1
    {"USR1", SIGUSR1},
#endif
#ifdef SIGUSR2
    {"USR2", SIGUSR2},
#endif
#ifdef SIGPIPE
    {"PIPE", SIGPIPE},
#endif
};

static bool smallclueParseSignal(const char *spec, int *out) {
    if (!spec || !*spec) {
        return false;
    }
    if (isdigit((unsigned char)spec[0])) {
        char *end = NULL;
        errno = 0;
        long val = strtol(spec, &end, 10);
        if (errno != 0 || !end || *end != '\0' || val <= 0 || val > NSIG) {
            return false;
        }
        *out = (int)val;
        return true;
    }
    if (spec[0] == 'S' && spec[1] == 'I' && spec[2] == 'G') {
        spec += 3;
    }
    for (size_t i = 0; i < sizeof(kSignalNames) / sizeof(kSignalNames[0]); ++i) {
        if (strcasecmp(spec, kSignalNames[i].name) == 0) {
            *out = kSignalNames[i].value;
            return true;
        }
    }
    return false;
}

static void smallclueKillListSignals(void) {
    for (size_t i = 0; i < sizeof(kSignalNames) / sizeof(kSignalNames[0]); ++i) {
        printf("%s ", kSignalNames[i].name);
    }
    putchar('\n');
}

static int smallclueKillCommandOriginal(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: kill [-SIGNAL] pid...\n");
        return 1;
    }
    int signo = SIGTERM;
    int idx = 1;
    if (idx < argc && argv[idx][0] == '-') {
        const char *spec = argv[idx] + 1;
        if (strcmp(spec, "l") == 0 || strcmp(spec, "L") == 0) {
            smallclueKillListSignals();
            return 0;
        }
        if (!smallclueParseSignal(spec, &signo)) {
            fprintf(stderr, "kill: invalid signal '%s'\n", spec);
            return 1;
        }
        idx++;
    }
    if (idx >= argc) {
        fprintf(stderr, "usage: kill [-SIGNAL] pid...\n");
        return 1;
    }
    int status = 0;
    for (; idx < argc; ++idx) {
        char *end = NULL;
        errno = 0;
        long pid_val = strtol(argv[idx], &end, 10);
        if (errno != 0 || !end || *end != '\0' || pid_val <= 0) {
            fprintf(stderr, "kill: invalid pid '%s'\n", argv[idx]);
            status = 1;
            continue;
        }
        if (kill((pid_t)pid_val, signo) != 0) {
            fprintf(stderr, "kill: %s: %s\n", argv[idx], strerror(errno));
            status = 1;
        }
    }
    return status;
}

static int smallclueKillCommand(int argc, char **argv) {
    return smallclueKillCommandOriginal(argc, argv);
}

static int smallclueXargsCommand(int argc, char **argv) {
    smallclueResetGetopt();
    int opt;
    while ((opt = getopt(argc, argv, "")) != -1) {
        fprintf(stderr, "usage: xargs command [initial-args]\n");
        return 1;
    }
    if (optind >= argc) {
        fprintf(stderr, "xargs: missing command name\n");
        return 1;
    }
    int base_count = argc - optind;
    char **base_args = &argv[optind];
    const SmallclueApplet *target = smallclueFindApplet(base_args[0]);
    if (!target) {
        fprintf(stderr, "xargs: '%s' not found\n", base_args[0]);
        return 127;
    }
    SmallclueLineVector extra = {0};
    if (!smallclueReadTokensFromStdin(&extra)) {
        perror("xargs");
        smallclueLineVectorFree(&extra);
        return 1;
    }
    size_t total = (size_t)base_count + extra.count;
    char **cmd_argv = (char **)calloc(total + 1, sizeof(char *));
    if (!cmd_argv) {
        perror("xargs");
        smallclueLineVectorFree(&extra);
        return 1;
    }
    size_t index = 0;
    for (int i = 0; i < base_count; ++i) {
        cmd_argv[index] = strdup(base_args[i]);
        if (!cmd_argv[index]) {
            perror("xargs");
            for (size_t k = 0; k < index; ++k) {
                free(cmd_argv[k]);
            }
            free(cmd_argv);
            smallclueLineVectorFree(&extra);
            return 1;
        }
        index++;
    }
    for (size_t i = 0; i < extra.count; ++i) {
        cmd_argv[index++] = extra.items[i];
        extra.items[i] = NULL;
    }
    int status = smallclueDispatchApplet(target, (int)total, cmd_argv);
    for (size_t i = 0; i < total; ++i) {
        free(cmd_argv[i]);
    }
    free(cmd_argv);
    smallclueLineVectorFree(&extra);
    return status;
}


static int smallclueLineVectorLoadStream(FILE *fp, const char *path, const char *cmd_name, SmallclueLineVector *vec) {
    char *line = NULL;
    size_t cap = 0;
    int status = 0;
    while (true) {
        ssize_t len = getline(&line, &cap, fp);
        if (len < 0) {
            if (feof(fp)) {
                break;
            }
            fprintf(stderr, "%s: %s: %s\n", cmd_name, path ? path : "(stdin)", strerror(errno));
            status = 1;
            break;
        }
        if (!smallclueLineVectorAppend(vec, line, (size_t)len)) {
            fprintf(stderr, "%s: %s: out of memory\n", cmd_name, path ? path : "(stdin)");
            status = 1;
            break;
        }
    }
    free(line);
    return status;
}

static int smallclueStringCompare(const void *a, const void *b) {
    const char *const *lhs = (const char *const *)a;
    const char *const *rhs = (const char *const *)b;
    return strcmp(*lhs, *rhs);
}

static int pagerCollectLines(const char *cmd_name, const char *path, FILE *stream, PagerBuffer *buffer) {
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t len;
    int status = 0;

    while ((len = getline(&line, &line_cap, stream)) != -1) {
        char *copy = (char *)malloc((size_t)len + 1);
        if (!copy) {
            fprintf(stderr, "%s: out of memory\n", pager_command_name(cmd_name));
            status = 1;
            break;
        }
        memcpy(copy, line, (size_t)len);
        copy[len] = '\0';
        if (!pagerBufferAppendLine(buffer, copy)) {
            free(copy);
            fprintf(stderr, "%s: out of memory\n", pager_command_name(cmd_name));
            status = 1;
            break;
        }
    }

    if (line) {
        free(line);
    }
    if (status == 0 && ferror(stream)) {
        fprintf(stderr, "%s: %s: %s\n", pager_command_name(cmd_name), path ? path : "(stdin)", strerror(errno));
        status = 1;
    }
    if (status != 0) {
        pagerBufferFree(buffer);
    }
    return status;
}

static void pagerRenderPage(const PagerBuffer *buffer, size_t start, int page_rows) {
    if (!buffer) {
        return;
    }
    if (page_rows < 1) {
        page_rows = 1;
    }
    fputs("\x1b[2J\x1b[H", stdout);
    size_t end = start + (size_t)page_rows;
    if (end > buffer->count) {
        end = buffer->count;
    }
    for (size_t i = start; i < end; ++i) {
        const char *line = buffer->lines[i];
        fputs(line, stdout);
        size_t len = strlen(line);
        if (len == 0 || line[len - 1] != '\n') {
            fputc('\n', stdout);
        }
    }
    fflush(stdout);
}

static size_t pagerMaxTop(const PagerBuffer *buffer, int page_rows) {
    if (!buffer || buffer->count == 0) {
        return 0;
    }
    size_t page = (size_t)(page_rows > 0 ? page_rows : 1);
    if (buffer->count <= page) {
        return 0;
    }
    return buffer->count - page;
}

static int pagerPromptAndRead(const char *cmd_name) {
    const char *label = pager_command_name(cmd_name);
    fprintf(stdout, "\r--%s-- (Space=next, b=prev, arrows=scroll, q=quit) ", label);
    fflush(stdout);
    int key = pager_read_key();
    fputs("\r\x1b[K", stdout);
    fflush(stdout);
    return key;
}

static int pagerInteractiveSession(const char *cmd_name, PagerBuffer *buffer, int page_rows) {
    if (!buffer || buffer->count == 0) {
        return 0;
    }
    if (page_rows < 1) {
        page_rows = 1;
    }

    size_t top = 0;
    bool redraw = true;
    while (1) {
        if (redraw) {
            pagerRenderPage(buffer, top, page_rows);
            redraw = false;
        }
        int key = pagerPromptAndRead(cmd_name);
        switch (key) {
            case 'q':
            case 'Q':
            case 3:
            case 4:
                return 0;
            case ' ':
            case PAGER_KEY_PAGE_DOWN: {
                size_t max_top = pagerMaxTop(buffer, page_rows);
                if (top < max_top) {
                    size_t new_top = top + (size_t)page_rows;
                    if (new_top > max_top) {
                        new_top = max_top;
                    }
                    top = new_top;
                    redraw = true;
                } else {
                    pagerBell();
                }
                break;
            }
            case 'b':
            case 'B':
            case PAGER_KEY_PAGE_UP: {
                if (top > 0) {
                    size_t delta = (size_t)page_rows;
                    if (delta > top) {
                        top = 0;
                    } else {
                        top -= delta;
                    }
                    redraw = true;
                } else {
                    pagerBell();
                }
                break;
            }
            case '\n':
            case '\r':
            case PAGER_KEY_ARROW_DOWN: {
                size_t page = (size_t)page_rows;
                if (top + page < buffer->count) {
                    top++;
                    redraw = true;
                } else {
                    pagerBell();
                }
                break;
            }
            case PAGER_KEY_ARROW_UP: {
                if (top > 0) {
                    top--;
                    redraw = true;
                } else {
                    pagerBell();
                }
                break;
            }
            default:
                // Ignore other keys
                break;
        }
    }
    return 0;
}

static int print_file(const char *path, FILE *stream) {
    char buffer[4096];
    while (!feof(stream)) {
        size_t n = fread(buffer, 1, sizeof(buffer), stream);
        if (n == 0) {
            break;
        }
        if (fwrite(buffer, 1, n, stdout) != n) {
            perror("cat: write error");
            return 1;
        }
    }
    if (ferror(stream)) {
        fprintf(stderr, "cat: %s: read error\n", path ? path : "(stdin)");
        return 1;
    }
    return 0;
}

static const char *pager_command_name(const char *name) {
    return (name && *name) ? name : "pager";
}

static int pager_control_fd_value = -2;

// Duplicate an FD for pager control input only if it can be read from.
static int pagerDupForRead(int fd) {
    int dup_fd = dup(fd);
    if (dup_fd < 0) {
        return -1;
    }
    int flags = fcntl(dup_fd, F_GETFL, 0);
    if (flags >= 0 && (flags & O_ACCMODE) != O_WRONLY) {
        return dup_fd;
    }
    close(dup_fd);
    return -1;
}

static int pager_control_fd(void) {
    if (pager_control_fd_value != -2) {
        return pager_control_fd_value;
    }
#ifdef _WIN32
    pager_control_fd_value = -1;
#else
    int fd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    if (fd < 0 && pscalRuntimeStdoutIsInteractive()) {
        const char *tty = ttyname(STDOUT_FILENO);
        if (tty && *tty) {
            fd = open(tty, O_RDONLY | O_CLOEXEC);
        }
        if (fd < 0) {
            fd = pagerDupForRead(STDOUT_FILENO);
        }
    }
    if (fd < 0 && pscalRuntimeStdinIsInteractive()) {
        const char *tty = ttyname(STDIN_FILENO);
        if (tty && *tty) {
            fd = open(tty, O_RDONLY | O_CLOEXEC);
        }
        if (fd < 0) {
            fd = pagerDupForRead(STDIN_FILENO);
        }
    }
    pager_control_fd_value = fd;
#endif
    return pager_control_fd_value;
}

static int pager_read_key(void) {
    int fd = pager_control_fd();
    if (fd < 0) {
        return 'q';
    }
    if (!pscalRuntimeFdIsInteractive(fd)) {
        return 'q';
    }
    struct termios orig;
    bool have_termios = (tcgetattr(fd, &orig) == 0);
    struct termios raw;
    if (have_termios) {
        raw = orig;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_iflag &= ~(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSAFLUSH, &raw);
    }
    int result = 'q';
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int poll_rc = poll(&pfd, 1, -1);
        if (poll_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if ((pfd.revents & POLLIN) == 0) {
            continue;
        }
        unsigned char ch = 0;
        ssize_t n = read(fd, &ch, 1);
        if (n <= 0) {
            break;
        }
        if (ch == '\x1b') {
            unsigned char seq[3] = {0};
            if (read(fd, &seq[0], 1) == 1 && seq[0] == '[') {
                if (read(fd, &seq[1], 1) == 1) {
                    if (seq[1] >= '0' && seq[1] <= '9') {
                        if (read(fd, &seq[2], 1) == 1 && seq[2] == '~') {
                            if (seq[1] == '5') {
                                result = PAGER_KEY_PAGE_UP;
                            } else if (seq[1] == '6') {
                                result = PAGER_KEY_PAGE_DOWN;
                            } else {
                                result = '\x1b';
                            }
                        } else {
                            result = '\x1b';
                        }
                    } else if (seq[1] == 'A') {
                        result = PAGER_KEY_ARROW_UP;
                    } else if (seq[1] == 'B') {
                        result = PAGER_KEY_ARROW_DOWN;
                    } else {
                        result = '\x1b';
                    }
                } else {
                    result = '\x1b';
                }
            } else {
                result = '\x1b';
            }
        } else {
            result = ch;
        }
        break;
    }
    if (have_termios) {
        tcsetattr(fd, TCSAFLUSH, &orig);
    }
    return result;
}

static int pager_terminal_rows(void) {
    struct winsize ws;
    if (isatty(STDOUT_FILENO) && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) {
            return ws.ws_row;
        }
    }
    int ctrl_fd = pager_control_fd();
    if (ctrl_fd >= 0 && ioctl(ctrl_fd, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) {
            return ws.ws_row;
        }
    }
    const char *lines = getenv("LINES");
    if (lines && *lines) {
        int parsed = atoi(lines);
        if (parsed > 0) {
            return parsed;
        }
    }
    return 24;
}

static int pager_file(const char *cmd_name, const char *path, FILE *stream) {
    PagerBuffer buffer = {0};
    if (pagerCollectLines(cmd_name, path, stream, &buffer) != 0) {
        return 1;
    }

    int ctrl_fd = pager_control_fd();
    bool have_ctrl = (ctrl_fd >= 0) && pscalRuntimeFdIsInteractive(ctrl_fd);
    if (!have_ctrl) {
        /* No interactive input available; dump what we collected. */
        for (size_t i = 0; i < buffer.count; ++i) {
            fputs(buffer.lines[i], stdout);
            size_t len = strlen(buffer.lines[i]);
            if (len == 0 || buffer.lines[i][len - 1] != '\n') {
                fputc('\n', stdout);
            }
        }
        pagerBufferFree(&buffer);
        return 0;
    }

    int rows = pager_terminal_rows();
    int page_rows = rows > 1 ? rows - 1 : rows;
    if (page_rows < 1) {
        page_rows = 1;
    }

    int status = pagerInteractiveSession(cmd_name, &buffer, page_rows);
    pagerBufferFree(&buffer);
    return status;
}

#define MARKDOWN_WRAP_WIDTH 78
#define MARKDOWN_MAX_TABLE_ROWS 500
#define MARKDOWN_MAX_TABLE_COLS 50
#define MARKDOWN_MIN_COL_WIDTH 10

typedef struct {
    char *cells[MARKDOWN_MAX_TABLE_COLS];
    int col_count;
} MarkdownTableRow;

typedef struct {
    char *name;
    char *title;
    char *path;
} MarkdownDocEntry;

static char *markdownExtractTitle(const char *path);

static void markdownDocEntryFree(MarkdownDocEntry *entry) {
    if (!entry) return;
    free(entry->name);
    free(entry->title);
    free(entry->path);
    entry->name = NULL;
    entry->title = NULL;
    entry->path = NULL;
}

static int markdownEnumerateDocuments(MarkdownDocEntry **entries_out,
                                      size_t *count_out,
                                      char *docs_dir_out,
                                      size_t docs_dir_len) {
    if (entries_out) *entries_out = NULL;
    if (count_out) *count_out = 0;
    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "md: HOME is not set\n");
        return 1;
    }
    char docs_dir[PATH_MAX];
    if (smallclueBuildPath(docs_dir, sizeof(docs_dir), home, "Docs") != 0) {
        fprintf(stderr, "md: unable to resolve Docs directory\n");
        return 1;
    }
    if (docs_dir_out && docs_dir_len > 0) {
        strncpy(docs_dir_out, docs_dir, docs_dir_len - 1);
        docs_dir_out[docs_dir_len - 1] = '\0';
    }
    DIR *dir = opendir(docs_dir);
    if (!dir) {
        fprintf(stderr, "md: %s: %s\n", docs_dir, strerror(errno));
        return 1;
    }
    MarkdownDocEntry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    struct dirent *dent;
    while ((dent = readdir(dir)) != NULL) {
        if (dent->d_name[0] == '.') {
            continue;
        }
        const char *dot = strrchr(dent->d_name, '.');
        if (!dot || strcasecmp(dot, ".md") != 0) {
            continue;
        }
        char path[PATH_MAX];
        if (smallclueBuildPath(path, sizeof(path), docs_dir, dent->d_name) != 0) {
            continue;
        }
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (count == capacity) {
            size_t new_cap = capacity == 0 ? 8 : capacity * 2;
            MarkdownDocEntry *resized = (MarkdownDocEntry *)realloc(entries, new_cap * sizeof(MarkdownDocEntry));
            if (!resized) {
                continue;
            }
            entries = resized;
            capacity = new_cap;
        }
        entries[count].name = strdup(dent->d_name);
        if (entries[count].name) {
            char *ext = strrchr(entries[count].name, '.');
            if (ext) *ext = '\0';
        }
        entries[count].title = markdownExtractTitle(path);
        entries[count].path = strdup(path);
        count++;
    }
    closedir(dir);
    if (entries_out) {
        *entries_out = entries;
    } else {
        for (size_t i = 0; i < count; ++i) {
            markdownDocEntryFree(&entries[i]);
        }
        free(entries);
    }
    if (count_out) {
        *count_out = count;
    }
    return 0;
}

static void markdownParagraphAppend(char **buffer, size_t *length, size_t *capacity, const char *text) {
    if (!text || !*text) {
        return;
    }
    size_t text_len = strlen(text);
    size_t needed = *length + text_len + 2;
    if (needed > *capacity) {
        size_t new_capacity = (*capacity == 0) ? 128 : *capacity * 2;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }
        char *resized = (char *)realloc(*buffer, new_capacity);
        if (!resized) {
            return;
        }
        *buffer = resized;
        *capacity = new_capacity;
    }
    if (*length > 0) {
        (*buffer)[(*length)++] = ' ';
    }
    memcpy(*buffer + *length, text, text_len);
    *length += text_len;
    (*buffer)[*length] = '\0';
}

static char *markdownSimplifyInline(const char *text) {
    if (!text) {
        return strdup("");
    }
    size_t len = strlen(text);
    size_t cap = len * 2 + 1;
    char *buffer = (char *)malloc(cap);
    if (!buffer) {
        return strdup(text);
    }
    size_t dst = 0;
    for (size_t i = 0; text[i]; ) {
        char ch = text[i];
        if (ch == '`') {
            i++;
            continue;
        }
        if ((ch == '*' || ch == '_')) {
            size_t advance = 1;
            if (text[i + 1] == ch) {
                advance = 2;
            }
            i += advance;
            continue;
        }
        if (ch == '[') {
            size_t close = i + 1;
            while (text[close] && text[close] != ']') {
                close++;
            }
            if (text[close] == ']' && text[close + 1] == '(') {
                size_t url_start = close + 2;
                size_t url_end = url_start;
                while (text[url_end] && text[url_end] != ')') {
                    url_end++;
                }
                if (text[url_end] == ')') {
                    for (size_t j = i + 1; j < close; ++j) {
                        buffer[dst++] = text[j];
                    }
                    if (url_end > url_start) {
                        buffer[dst++] = ' ';
                        buffer[dst++] = '(';
                        for (size_t j = url_start; j < url_end; ++j) {
                            buffer[dst++] = text[j];
                        }
                        buffer[dst++] = ')';
                    }
                    i = url_end + 1;
                    continue;
                }
            }
        }
        buffer[dst++] = ch;
        i++;
    }
    buffer[dst] = '\0';
    return buffer;
}

static void markdownWrapAndWrite(FILE *out, const char *text, const char *firstPrefix, const char *subPrefix, int width) {
    if (!out) {
        return;
    }
    if (!text || !*text) {
        if (firstPrefix && *firstPrefix) {
            fputs(firstPrefix, out);
        }
        fputc('\n', out);
        return;
    }
    const char *prefix_first = firstPrefix ? firstPrefix : "";
    const char *prefix_sub = subPrefix ? subPrefix : prefix_first;
    size_t prefix_first_len = strlen(prefix_first);
    size_t prefix_sub_len = strlen(prefix_sub);
    int wrap_width = width > 20 ? width : MARKDOWN_WRAP_WIDTH;

    char *copy = strdup(text);
    if (!copy) {
        return;
    }
    char *saveptr = NULL;
    char *token = strtok_r(copy, " \t\r\n", &saveptr);
    size_t current = prefix_first_len;
    fputs(prefix_first, out);
    bool first_word = true;
    while (token) {
        size_t tok_len = strlen(token);
        size_t extra = first_word ? tok_len : tok_len + 1;
        if (!first_word && (int)(current + extra) > wrap_width) {
            fputc('\n', out);
            fputs(prefix_sub, out);
            current = prefix_sub_len;
            first_word = true;
            extra = tok_len;
        }
        if (!first_word) {
            fputc(' ', out);
            current++;
        }
        fputs(token, out);
        current += tok_len;
        first_word = false;
        token = strtok_r(NULL, " \t\r\n", &saveptr);
    }
    fputc('\n', out);
    free(copy);
}

static int markdownTermWidth(void) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return w.ws_col;
    }
    const char *cols = getenv("COLUMNS");
    if (cols && *cols) {
        int parsed = atoi(cols);
        if (parsed > 0) {
            return parsed;
        }
    }
    return MARKDOWN_WRAP_WIDTH;
}

static char *markdownTrimInline(char *str) {
    if (!str) return str;
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    char *end = str + strlen(str);
    while (end > str && isspace((unsigned char)*(end - 1))) {
        *(--end) = '\0';
    }
    return str;
}

static void markdownParseTableRow(char *line, MarkdownTableRow *row) {
    if (!row) return;
    row->col_count = 0;
    if (!line) return;
    char *cursor = line;
    if (*cursor == '|') {
        cursor++;
    }
    char *start = cursor;
    while (*cursor) {
        if (*cursor == '|') {
            *cursor = '\0';
            if (row->col_count < MARKDOWN_MAX_TABLE_COLS) {
                row->cells[row->col_count++] = strdup(markdownTrimInline(start));
            }
            start = cursor + 1;
        }
        cursor++;
    }
    if (*start) {
        if (row->col_count < MARKDOWN_MAX_TABLE_COLS) {
            row->cells[row->col_count++] = strdup(markdownTrimInline(start));
        }
    }
}

static int markdownIsSeparatorRow(const MarkdownTableRow *row) {
    if (!row || row->col_count == 0) return 0;
    const char *cell = row->cells[0];
    if (!cell) return 0;
    int has_dash = 0;
    for (const char *p = cell; *p; ++p) {
        if (*p == '-') {
            has_dash = 1;
        } else if (*p == ':' || isspace((unsigned char)*p)) {
            continue;
        } else {
            return 0;
        }
    }
    return has_dash;
}

static int markdownTableWrapLen(const char *text, int max_width, int offset) {
    int len = (int)strlen(text);
    int remaining = len - offset;
    if (remaining <= max_width) return remaining;
    for (int k = max_width; k > 0; --k) {
        char prev = text[offset + k - 1];
        if (prev == ' ') return k;
        if (prev == '/' || prev == '-' || prev == '_' || prev == ',' || prev == '.') {
            return k;
        }
    }
    return max_width;
}

static void markdownPrintSeparator(FILE *out, const int *col_widths, int cols) {
    fputs("  +", out);
    for (int j = 0; j < cols; ++j) {
        for (int k = 0; k < col_widths[j] + 2; ++k) {
            fputc('-', out);
        }
        fputc('+', out);
    }
    fputc('\n', out);
}

static void markdownRenderTable(FILE *out, MarkdownTableRow *rows, int row_count) {
    if (!rows || row_count <= 0) return;
    if (!out) return;

    int term_width = markdownTermWidth();
    int col_widths[MARKDOWN_MAX_TABLE_COLS] = {0};
    int max_cols = 0;

    for (int i = 0; i < row_count; ++i) {
        if (rows[i].col_count > max_cols) {
            max_cols = rows[i].col_count;
        }
        if (!markdownIsSeparatorRow(&rows[i])) {
            for (int j = 0; j < rows[i].col_count; ++j) {
                int len = (int)strlen(rows[i].cells[j]);
                if (len > col_widths[j]) {
                    col_widths[j] = len;
                }
            }
        }
    }

    int total_padding = (max_cols * 3) + 1;
    int available = term_width - total_padding;
    int current_total = 0;
    for (int j = 0; j < max_cols; ++j) current_total += col_widths[j];
    if (available > 0 && current_total > available && max_cols > 0) {
        int avg = available / max_cols;
        if (avg < MARKDOWN_MIN_COL_WIDTH) avg = MARKDOWN_MIN_COL_WIDTH;
        for (int j = 0; j < max_cols; ++j) {
            if (col_widths[j] > avg) col_widths[j] = avg;
        }
    }

    markdownPrintSeparator(out, col_widths, max_cols);
    for (int i = 0; i < row_count; ++i) {
        if (markdownIsSeparatorRow(&rows[i])) {
            markdownPrintSeparator(out, col_widths, max_cols);
            continue;
        }
        int offsets[MARKDOWN_MAX_TABLE_COLS] = {0};
        bool done = false;
        while (!done) {
            done = true;
            fputs("  |", out);
            for (int j = 0; j < max_cols; ++j) {
                const char *text = (j < rows[i].col_count && rows[i].cells[j]) ? rows[i].cells[j] : "";
                int width = col_widths[j];
                int len = (int)strlen(text);
                int off = offsets[j];
                if (off < len) {
                    done = false;
                    int take = markdownTableWrapLen(text, width, off);
                    fprintf(out, " %-*.*s", width, take, text + off);
                    offsets[j] += take;
                    if (offsets[j] < len && text[offsets[j]] == ' ') {
                        offsets[j]++;
                    }
                } else {
                    fprintf(out, " %-*s", width, "");
                }
                fputs(" |", out);
            }
            fputc('\n', out);
        }
    }
    markdownPrintSeparator(out, col_widths, max_cols);
    fputc('\n', out);

    for (int i = 0; i < row_count; ++i) {
        for (int j = 0; j < rows[i].col_count; ++j) {
            free(rows[i].cells[j]);
            rows[i].cells[j] = NULL;
        }
    }
}

static bool markdownIsHorizontalRule(const char *text) {
    if (!text) return false;
    size_t dash_count = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == '-' || *p == '_' || *p == '*') {
            dash_count++;
        } else if (!isspace((unsigned char)*p)) {
            return false;
        }
    }
    return dash_count >= 3;
}

static int markdownHeadingLevel(const char *text) {
    if (!text) return 0;
    int level = 0;
    const char *p = text;
    while (*p == '#') {
        level++;
        p++;
    }
    if (level > 0 && (*p == ' ' || *p == '\t')) {
        return level;
    }
    return 0;
}

static void markdownWriteHeading(FILE *out, const char *text, int level) {
    if (!text || !*text) return;
    char *formatted = markdownSimplifyInline(text);
    if (!formatted) return;
    for (char *p = formatted; *p; ++p) {
        *p = (char)toupper((unsigned char)*p);
    }
    fprintf(out, "%s\n", formatted);
    char underline = (level == 1) ? '=' : '-';
    size_t len = strlen(formatted);
    size_t underline_len = len > MARKDOWN_WRAP_WIDTH ? MARKDOWN_WRAP_WIDTH : len;
    for (size_t i = 0; i < underline_len; ++i) {
        fputc(underline, out);
    }
    fputc('\n', out);
    fputc('\n', out);
    free(formatted);
}

static void markdownBuildPrefix(char *buffer, size_t buffer_size, int spaces, const char *suffix) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    if (spaces < 0) spaces = 0;
    if ((size_t)(spaces + 1) > buffer_size) {
        spaces = (int)buffer_size - 1;
    }
    size_t suffix_len = suffix ? strlen(suffix) : 0;
    if ((size_t)spaces + suffix_len + 1 > buffer_size) {
        if (suffix_len + 1 > buffer_size) {
            suffix_len = buffer_size - 1;
        }
        spaces = 0;
    }
    memset(buffer, ' ', (size_t)spaces);
    if (suffix_len > 0) {
        memcpy(buffer + spaces, suffix, suffix_len);
    }
    buffer[spaces + suffix_len] = '\0';
}

static bool markdownExtractListItem(char *line, char **content, char *firstPrefix, size_t firstSize, char *subPrefix, size_t subSize) {
    if (!line || !content || !firstPrefix || !subPrefix) {
        return false;
    }
    char *p = line;
    int indent = 0;
    while (*p == ' ' || *p == '\t') {
        indent += (*p == '\t') ? 4 : 1;
        p++;
    }
    if ((*p == '-' || *p == '*' || *p == '+') && (p[1] == ' ' || p[1] == '\t')) {
        p++;
        while (*p == ' ' || *p == '\t') p++;
        *content = p;
        markdownBuildPrefix(firstPrefix, firstSize, indent, "• ");
        markdownBuildPrefix(subPrefix, subSize, indent + 2, "  ");
        return true;
    }
    if (isdigit((unsigned char)*p)) {
        const char *start = p;
        while (isdigit((unsigned char)*p)) {
            p++;
        }
        if (*p == '.' && (p[1] == ' ' || p[1] == '\t')) {
            int value = atoi(start);
            p++;
            while (*p == ' ' || *p == '\t') p++;
            *content = p;
            char suffix[16];
            snprintf(suffix, sizeof(suffix), "%d. ", value);
            markdownBuildPrefix(firstPrefix, firstSize, indent, suffix);
            size_t suffix_len = strlen(suffix);
            markdownBuildPrefix(subPrefix, subSize, indent + (int)suffix_len, "  ");
            return true;
        }
    }
    return false;
}

static void markdownFlushParagraph(FILE *out, char **paragraph, size_t *length) {
    if (!paragraph || !*paragraph || !length || *length == 0) {
        return;
    }
    (*paragraph)[*length] = '\0';
    const char *text = *paragraph;
    /* Naive table detection: look for '|' separators with at least two columns. */
    int pipe_count = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == '|') {
            pipe_count++;
        }
    }
    if (pipe_count >= 2 && strchr(text, '\n') == NULL) {
        /* Single-line paragraph that looks like a table row; just print it. */
        fprintf(out, "%s\n\n", text);
    } else {
        char *formatted = markdownSimplifyInline(text);
        if (formatted) {
            markdownWrapAndWrite(out, formatted, "", "", MARKDOWN_WRAP_WIDTH);
            free(formatted);
        }
        fputc('\n', out);
    }
    *length = 0;
    **paragraph = '\0';
}

static int markdownRenderStream(const char *label, FILE *input, FILE *output) {
    if (!input || !output) {
        return 1;
    }
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    bool in_code_block = false;
    bool in_table = false;
    char *paragraph = NULL;
    size_t paragraph_len = 0;
    size_t paragraph_cap = 0;
    MarkdownTableRow table_rows[MARKDOWN_MAX_TABLE_ROWS];
    int table_row_count = 0;

    if (label && *label) {
        fprintf(output, "%s\n", label);
        size_t underline_len = strlen(label);
        if (underline_len > MARKDOWN_WRAP_WIDTH) {
            underline_len = MARKDOWN_WRAP_WIDTH;
        }
        for (size_t i = 0; i < underline_len; ++i) {
            fputc('=', output);
        }
        fputc('\n', output);
        fputc('\n', output);
    }

    while ((line_len = getline(&line, &line_cap, input)) != -1) {
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }
        for (char *p = line; *p; ++p) {
            if (*p == '\t') {
                *p = ' ';
            }
        }
        char *trimmed = line;
        while (*trimmed && isspace((unsigned char)*trimmed)) {
            trimmed++;
        }
        char *end = trimmed + strlen(trimmed);
        while (end > trimmed && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }

        if (strncmp(trimmed, "```", 3) == 0) {
            markdownFlushParagraph(output, &paragraph, &paragraph_len);
            in_code_block = !in_code_block;
            continue;
        }

        if (in_code_block) {
            /* Preserve leading whitespace/tabs inside fenced code blocks. */
            fprintf(output, "    %s\n", line);
            continue;
        }

        if (*trimmed == '\0') {
            if (in_table) {
                markdownRenderTable(output, table_rows, table_row_count);
                table_row_count = 0;
                in_table = false;
            }
            markdownFlushParagraph(output, &paragraph, &paragraph_len);
            fputc('\n', output);
            continue;
        }

        if (markdownIsHorizontalRule(trimmed)) {
            markdownFlushParagraph(output, &paragraph, &paragraph_len);
            for (int i = 0; i < MARKDOWN_WRAP_WIDTH; ++i) {
                fputc('-', output);
            }
            fputc('\n', output);
            fputc('\n', output);
            continue;
        }

        int heading = markdownHeadingLevel(trimmed);
        if (heading > 0) {
            if (in_table) {
                markdownRenderTable(output, table_rows, table_row_count);
                table_row_count = 0;
                in_table = false;
            }
            markdownFlushParagraph(output, &paragraph, &paragraph_len);
            const char *heading_text = trimmed + heading;
            while (*heading_text == ' ' || *heading_text == '\t') heading_text++;
            markdownWriteHeading(output, heading_text, heading);
            continue;
        }

        if (*trimmed == '>') {
            if (in_table) {
                markdownRenderTable(output, table_rows, table_row_count);
                table_row_count = 0;
                in_table = false;
            }
            markdownFlushParagraph(output, &paragraph, &paragraph_len);
            char *quote = trimmed + 1;
            while (*quote == ' ' || *quote == '\t') quote++;
            char *formatted = markdownSimplifyInline(quote);
            if (formatted) {
                markdownWrapAndWrite(output, formatted, "> ", "> ", MARKDOWN_WRAP_WIDTH);
                free(formatted);
            }
            fputc('\n', output);
            continue;
        }

        int pipe_count = 0;
        for (const char *p = trimmed; *p; ++p) {
            if (*p == '|') {
                pipe_count++;
            }
        }
        if (pipe_count >= 2) {
            if (!in_table) {
                markdownFlushParagraph(output, &paragraph, &paragraph_len);
                in_table = true;
                table_row_count = 0;
            }
            if (table_row_count < MARKDOWN_MAX_TABLE_ROWS) {
                char *dup = strdup(trimmed);
                if (dup) {
                    markdownParseTableRow(dup, &table_rows[table_row_count]);
                    free(dup);
                    table_row_count++;
                }
            }
            continue;
        } else if (in_table) {
            markdownRenderTable(output, table_rows, table_row_count);
            table_row_count = 0;
            in_table = false;
        }

        char *list_text = NULL;
        char prefix_first[32];
        char prefix_sub[32];
        if (markdownExtractListItem(trimmed, &list_text, prefix_first, sizeof(prefix_first), prefix_sub, sizeof(prefix_sub))) {
            markdownFlushParagraph(output, &paragraph, &paragraph_len);
            char *formatted = markdownSimplifyInline(list_text);
            if (formatted) {
                markdownWrapAndWrite(output, formatted, prefix_first, prefix_sub, MARKDOWN_WRAP_WIDTH);
                free(formatted);
            }
            continue;
        }

        markdownParagraphAppend(&paragraph, &paragraph_len, &paragraph_cap, trimmed);
    }

    markdownFlushParagraph(output, &paragraph, &paragraph_len);
    if (in_table) {
        markdownRenderTable(output, table_rows, table_row_count);
    }
    free(paragraph);
    free(line);
    return 0;
}

static int markdownResolvePath(const char *input, char *resolved, size_t resolved_size) {
    if (!input || !resolved || resolved_size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(input, "-") == 0) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (stat(input, &st) == 0 && S_ISREG(st.st_mode)) {
        if (realpath(input, resolved)) {
            return 0;
        }
        strncpy(resolved, input, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
        return 0;
    }
    if (strchr(input, '/')) {
        return -1;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        return -1;
    }
    char docs_directory[PATH_MAX];
    if (smallclueBuildPath(docs_directory, sizeof(docs_directory), home, "Docs") != 0) {
        return -1;
    }
    char candidate[PATH_MAX];
    if (smallclueBuildPath(candidate, sizeof(candidate), docs_directory, input) == 0 &&
        stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
        strncpy(resolved, candidate, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
        return 0;
    }
    char with_ext[PATH_MAX];
    snprintf(with_ext, sizeof(with_ext), "%s.md", input);
    if (smallclueBuildPath(candidate, sizeof(candidate), docs_directory, with_ext) == 0 &&
        stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
        strncpy(resolved, candidate, resolved_size - 1);
        resolved[resolved_size - 1] = '\0';
        return 0;
    }
    return -1;
}

static int smallclueMarkdownDisplayStream(const char *label, FILE *input) {
    FILE *buffer = tmpfile();
    bool direct = false;
    if (!buffer) {
        buffer = stdout;
        direct = true;
    }
    if (markdownRenderStream(label, input, buffer) != 0) {
        if (!direct) {
            fclose(buffer);
        }
        return 1;
    }
    if (direct) {
        fflush(buffer);
        return 0;
    }
    fflush(buffer);
    rewind(buffer);
    int status = pager_file("md", label ? label : "(stdin)", buffer);
    fclose(buffer);
    return status;
}

static int smallclueMarkdownDisplayPath(const char *path) {
    if (!path) {
        return 1;
    }
    if (strcmp(path, "-") == 0) {
        return smallclueMarkdownDisplayStream("(stdin)", stdin);
    }
    char resolved[PATH_MAX];
    if (markdownResolvePath(path, resolved, sizeof(resolved)) != 0) {
        fprintf(stderr, "md: %s: %s\n", path, strerror(errno));
        return 1;
    }
    FILE *fp = fopen(resolved, "r");
    if (!fp) {
        fprintf(stderr, "md: %s: %s\n", resolved, strerror(errno));
        return 1;
    }
    int status = smallclueMarkdownDisplayStream(resolved, fp);
    fclose(fp);
    return status;
}

static char *markdownExtractTitle(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return NULL;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    char *title = NULL;
    while ((len = getline(&line, &cap, fp)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        char *trimmed = line;
        while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
        if (*trimmed == '\0') {
            continue;
        }
        if (trimmed[0] == '#') {
            while (*trimmed == '#') trimmed++;
            if (*trimmed == ' ' || *trimmed == '\t') trimmed++;
            char *formatted = markdownSimplifyInline(trimmed);
            title = formatted;
            break;
        }
    }
    free(line);
    fclose(fp);
    if (!title) {
        title = strdup("(untitled)");
    }
    return title;
}

static int markdownDocEntryCompare(const void *a, const void *b) {
    const MarkdownDocEntry *left = (const MarkdownDocEntry *)a;
    const MarkdownDocEntry *right = (const MarkdownDocEntry *)b;
    if (!left->name || !right->name) {
        return 0;
    }
    return strcasecmp(left->name, right->name);
}

static int smallclueMarkdownListDocuments(void) {
    MarkdownDocEntry *entries = NULL;
    size_t count = 0;
    char docs_dir[PATH_MAX];
    if (markdownEnumerateDocuments(&entries, &count, docs_dir, sizeof(docs_dir)) != 0) {
        return 1;
    }
    if (count == 0) {
        printf("No Markdown documents found in %s\n", docs_dir);
        free(entries);
        return 1;
    }
    qsort(entries, count, sizeof(MarkdownDocEntry), markdownDocEntryCompare);
    printf("Markdown documents in %s:\n\n", docs_dir);
    for (size_t i = 0; i < count; ++i) {
        const char *title = entries[i].title ? entries[i].title : "";
        printf("  %-24s %s\n", entries[i].name ? entries[i].name : "(unknown)", title);
        markdownDocEntryFree(&entries[i]);
    }
    free(entries);
    return 0;
}

static void markdownInteractiveRenderList(MarkdownDocEntry *entries,
                                          size_t count,
                                          size_t top,
                                          size_t cursor,
                                          size_t window,
                                          const char *docs_dir) {
    printf("\x1b[2J\x1b[H");
    printf("Markdown documents (Arrow keys to navigate, Enter to open, q to quit)\n\n");
    size_t end = top + window;
    if (end > count) {
        end = count;
    }
    for (size_t idx = top; idx < end; ++idx) {
        bool highlight = (idx == cursor);
        if (highlight) {
            printf("\x1b[7m");
        } else {
            printf(" ");
        }
        const char *name = entries[idx].name ? entries[idx].name : "(unknown)";
        const char *title = entries[idx].title ? entries[idx].title : "";
        printf(" %-24s %s", name, title);
        if (highlight) {
            printf("\x1b[0m");
        }
        printf("\n");
    }
    printf("\nDocs directory: %s\n", docs_dir);
    fflush(stdout);
}

static int markdownInteractiveSelectDocument(void) {
    MarkdownDocEntry *entries = NULL;
    size_t count = 0;
    char docs_dir[PATH_MAX];
    if (markdownEnumerateDocuments(&entries, &count, docs_dir, sizeof(docs_dir)) != 0) {
        return 1;
    }
    if (count == 0) {
        printf("No Markdown documents found in %s\n", docs_dir);
        free(entries);
        return 1;
    }
    qsort(entries, count, sizeof(MarkdownDocEntry), markdownDocEntryCompare);
    if (!pscalRuntimeStdoutIsInteractive()) {
        printf("Markdown documents in %s:\n\n", docs_dir);
        for (size_t i = 0; i < count; ++i) {
            const char *title = entries[i].title ? entries[i].title : "";
            printf("  %-24s %s\n", entries[i].name ? entries[i].name : "(unknown)", title);
            markdownDocEntryFree(&entries[i]);
        }
        free(entries);
        return 0;
    }

    size_t cursor = 0;
    size_t top = 0;
    int rows = pager_terminal_rows();
    size_t window = (rows > 6) ? (size_t)(rows - 4) : (count < 4 ? count : 4);
    if (window == 0) window = 1;
    if (window > count) window = count;
    char selected[PATH_MAX];
    bool running = true;
    bool has_selection = false;

    while (running) {
        if (cursor < top) {
            top = cursor;
        } else if (cursor >= top + window) {
            top = cursor - window + 1;
        }

        markdownInteractiveRenderList(entries, count, top, cursor, window, docs_dir);

        int key = pager_read_key();
        switch (key) {
            case PAGER_KEY_ARROW_DOWN:
                if (cursor + 1 < count) {
                    cursor++;
                } else {
                    pagerBell();
                }
                break;
            case PAGER_KEY_ARROW_UP:
                if (cursor > 0) {
                    cursor--;
                } else {
                    pagerBell();
                }
                break;
            case PAGER_KEY_PAGE_DOWN:
            case ' ':
                if (cursor + window < count) {
                    cursor += window;
                } else if (cursor != count - 1) {
                    cursor = count - 1;
                } else {
                    pagerBell();
                }
                break;
            case PAGER_KEY_PAGE_UP:
                if (cursor >= window) {
                    cursor -= window;
                } else if (cursor != 0) {
                    cursor = 0;
                } else {
                    pagerBell();
                }
                break;
            case '\r':
            case '\n':
                if (entries[cursor].path) {
                    strncpy(selected, entries[cursor].path, sizeof(selected) - 1);
                    selected[sizeof(selected) - 1] = '\0';
                    has_selection = true;
                }
                running = false;
                break;
            case 'q':
            case 'Q':
            case 0x03: // Ctrl-C
                running = false;
                break;
            default:
                break;
        }
    }

    printf("\x1b[2J\x1b[H");
    fflush(stdout);

    for (size_t i = 0; i < count; ++i) {
        markdownDocEntryFree(&entries[i]);
    }
    free(entries);

    if (!has_selection) {
        return 0;
    }
    int status = smallclueMarkdownDisplayPath(selected);
    if (status == 0) {
        // After viewing a document, return to the list.
        return markdownInteractiveSelectDocument();
    }
    return status;
}

#if defined(PSCAL_HAS_LIBCURL)
static size_t smallclueCurlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    FILE *dest = (FILE *)userp;
    size_t bytes = size * nmemb;
    if (bytes == 0) return 0;
    if (fwrite(contents, 1, bytes, dest) != bytes) {
        return 0;
    }
    return bytes;
}
#endif

static void smallclueUrlSuggestFilename(const char *url, char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    buffer[0] = '\0';
    if (!url || !*url) {
        strncpy(buffer, "index.html", buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return;
    }
    const char *start = strstr(url, "://");
    start = start ? start + 3 : url;
    const char *leaf = strrchr(start, '/');
    leaf = (leaf && leaf[1] != '\0') ? leaf + 1 : start;
    while (*leaf == '/') {
        leaf++;
    }
    const char *end = leaf;
    while (*end && *end != '?' && *end != '#') {
        end++;
    }
    size_t len = (size_t)(end - leaf);
    if (len == 0) {
        strncpy(buffer, "index.html", buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        return;
    }
    if (len >= buffer_size) {
        len = buffer_size - 1;
    }
    memcpy(buffer, leaf, len);
    buffer[len] = '\0';
}

static int smallclueHttpFetch(const char *cmd_name, const char *url, const char *destinationPath) {
#if !defined(PSCAL_HAS_LIBCURL)
    fprintf(stderr, "%s: networking support is unavailable in this build.\n", cmd_name ? cmd_name : "curl");
    return 1;
#else
    if (!url || !*url) {
        fprintf(stderr, "%s: missing URL\n", cmd_name ? cmd_name : "curl");
        return 1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "%s: failed to initialise libcurl\n", cmd_name ? cmd_name : "curl");
        return 1;
    }
    FILE *dest = stdout;
    bool close_dest = false;
    if (destinationPath && destinationPath[0] != '\0' && strcmp(destinationPath, "-") != 0) {
        dest = fopen(destinationPath, "wb");
        if (!dest) {
            fprintf(stderr, "%s: %s: %s\n", cmd_name ? cmd_name : "curl", destinationPath, strerror(errno));
            curl_easy_cleanup(curl);
            return 1;
        }
        close_dest = true;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "smallclue-http/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, smallclueCurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, dest);
    CURLcode res = curl_easy_perform(curl);
    if (close_dest) {
        fclose(dest);
    } else {
        fflush(dest);
    }
    if (res != CURLE_OK) {
        fprintf(stderr, "%s: %s: %s\n", cmd_name ? cmd_name : "curl", url, curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return 1;
    }
    curl_easy_cleanup(curl);
    return 0;
#endif
}

static int cat_file(const char *path) {
    int status = 0;
    if (!path || strcmp(path, "-") == 0) {
        return print_file("(stdin)", stdin);
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cat: %s: %s\n", path, strerror(errno));
        return 1;
    }
    status = print_file(path, fp);
    fclose(fp);
    return status;
}

static void print_permissions(mode_t mode) {
    putchar(S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-');
    putchar(mode & S_IRUSR ? 'r' : '-');
    putchar(mode & S_IWUSR ? 'w' : '-');
    putchar(mode & S_IXUSR ? 'x' : '-');
    putchar(mode & S_IRGRP ? 'r' : '-');
    putchar(mode & S_IWGRP ? 'w' : '-');
    putchar(mode & S_IXGRP ? 'x' : '-');
    putchar(mode & S_IROTH ? 'r' : '-');
    putchar(mode & S_IWOTH ? 'w' : '-');
    putchar(mode & S_IXOTH ? 'x' : '-');
}

static void print_long_listing(const char *filename, const struct stat *s, bool human) {
    print_permissions(s->st_mode);
    printf(" %2llu", (unsigned long long)s->st_nlink);

    struct passwd *pw = getpwuid(s->st_uid);
    printf(" %-8s", pw ? pw->pw_name : "?");

    struct group *gr = getgrgid(s->st_gid);
    printf(" %-8s", gr ? gr->gr_name : "?");

    if (human) {
        char sizebuf[32];
        smallclueDfFormatSize(sizebuf, sizeof(sizebuf), (unsigned long long)s->st_size, true);
        printf(" %8s", sizebuf);
    } else {
        printf(" %8lld", (long long)s->st_size);
    }
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%b %d %H:%M", localtime(&s->st_mtime));
    printf(" %s %s", time_buf, filename);

    if (S_ISLNK(s->st_mode)) {
        char link_target[1024];
        ssize_t len = readlink(filename, link_target, sizeof(link_target) - 1);
        if (len >= 0) {
            link_target[len] = '\0';
            printf(" -> %s", link_target);
        }
    }
    putchar('\n');
}

static int print_path_entry_with_stat(const char *path,
                                      const char *label,
                                      bool long_format,
                                      bool human,
                                      const struct stat *stat_buf,
                                      int color_mode,
                                      int classify) {
    struct stat local_stat;
    const struct stat *st = stat_buf;
    if (!st) {
        if (lstat(path, &local_stat) == -1) {
            fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
            return 1;
        }
        st = &local_stat;
    }

    int color = 0;
    if (color_mode > 0 && st) {
        if (S_ISDIR(st->st_mode)) color = 34;       /* blue */
        else if (S_ISLNK(st->st_mode)) color = 36;  /* cyan */
        else if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode)) color = 33; /* yellow */
        else if (st->st_mode & S_IXUSR) color = 32; /* green executables */
    }

    char decorated[PATH_MAX];
    const char *display = label ? label : path;
    const char *out = display;
    if (classify && st) {
        char suffix = '\0';
        if (S_ISDIR(st->st_mode)) suffix = '/';
        else if (S_ISLNK(st->st_mode)) suffix = '@';
        else if (S_ISSOCK(st->st_mode)) suffix = '=';
        else if (S_ISFIFO(st->st_mode)) suffix = '|';
        else if (st->st_mode & S_IXUSR) suffix = '*';
        if (suffix != '\0') {
            snprintf(decorated, sizeof(decorated), "%s%c", display, suffix);
            decorated[sizeof(decorated) - 1] = '\0';
            out = decorated;
        }
    }
    if (long_format) {
        if (color)
            printf("\033[%dm", color);
        print_long_listing(out, st, human);
        if (color)
            printf("\033[0m");
    } else {
        if (color)
            printf("\033[%dm%s\033[0m\n", color, out);
        else
            printf("%s\n", out);
    }
    return 0;
}

static char *join_path(const char *base, const char *name) {
    if (!base || !*base || strcmp(base, ".") == 0) {
        return strdup(name);
    }
    size_t base_len = strlen(base);
    bool needs_sep = base_len > 0 && base[base_len - 1] != '/';
    size_t total = base_len + strlen(name) + (needs_sep ? 2 : 1);
    char *joined = (char *)malloc(total);
    if (!joined) {
        return NULL;
    }
    strcpy(joined, base);
    if (needs_sep) {
        strcat(joined, "/");
    }
    strcat(joined, name);
    return joined;
}

static int print_path_entry(const char *path, const char *label, bool long_format, bool human, int color_mode, int classify) {
    return print_path_entry_with_stat(path, label, long_format, human, NULL, color_mode, classify);
}

typedef struct {
    char *name;
    char *full_path;
    struct stat stat_buf;
} SmallclueLsEntry;

static void free_ls_entries(SmallclueLsEntry *entries, size_t count) {
    if (!entries) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(entries[i].name);
        free(entries[i].full_path);
    }
    free(entries);
}

static int compare_ls_entries_by_mtime(const void *lhs, const void *rhs) {
    const SmallclueLsEntry *a = (const SmallclueLsEntry *)lhs;
    const SmallclueLsEntry *b = (const SmallclueLsEntry *)rhs;
    if (a->stat_buf.st_mtime > b->stat_buf.st_mtime) {
        return -1;
    }
    if (a->stat_buf.st_mtime < b->stat_buf.st_mtime) {
        return 1;
    }
    return strcmp(a->name, b->name);
}

static void print_ls_columns(const SmallclueLsEntry *entries, size_t count, int color_mode, int classify) {
    if (count == 0) {
        return;
    }

    int term_cols = pscalRuntimeDetectWindowCols();
    if (term_cols <= 0) {
        term_cols = 80;
    }

    size_t max_len = 0;
    for (size_t i = 0; i < count; ++i) {
        size_t len = strlen(entries[i].name);
        if (len > max_len) {
            max_len = len;
        }
    }

    size_t col_width = max_len + 2; /* padding between columns */
    if (col_width == 0) {
        return;
    }

    int cols = term_cols / (int)col_width;
    if (cols < 1) {
        cols = 1;
    }
    int rows = (int)((count + (size_t)cols - 1) / (size_t)cols);

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            size_t idx = (size_t)c * (size_t)rows + (size_t)r;
            if (idx >= count) {
                continue;
            }
            const struct stat *st = &entries[idx].stat_buf;
            const char *name = entries[idx].name;
            char decorated[PATH_MAX];
            const char *out = name;
            if (classify) {
                char suffix = '\0';
                if (S_ISDIR(st->st_mode)) suffix = '/';
                else if (S_ISLNK(st->st_mode)) suffix = '@';
                else if (S_ISSOCK(st->st_mode)) suffix = '=';
                else if (S_ISFIFO(st->st_mode)) suffix = '|';
                else if (st->st_mode & S_IXUSR) suffix = '*';
                if (suffix != '\0') {
                    snprintf(decorated, sizeof(decorated), "%s%c", name, suffix);
                    decorated[sizeof(decorated) - 1] = '\0';
                    out = decorated;
                }
            }
            int color = 0;
            if (color_mode > 0) {
                if (S_ISDIR(st->st_mode)) color = 34;
                else if (S_ISLNK(st->st_mode)) color = 36;
                else if (S_ISCHR(st->st_mode) || S_ISBLK(st->st_mode)) color = 33;
                else if (st->st_mode & S_IXUSR) color = 32;
            }
            if (c == cols - 1 || (size_t)((c + 1) * rows + r) >= count) {
                if (color) printf("\033[%dm%s\033[0m", color, out); else printf("%s", out);
            } else {
                if (color) printf("\033[%dm%-*s\033[0m", color, (int)col_width, out);
                else printf("%-*s", (int)col_width, out);
            }
        }
        putchar('\n');
    }
}

static int list_directory(const char *path,
                          bool show_all,
                          bool long_format,
                          bool sort_by_time,
                          bool human,
                          int color_mode,
                          int classify) {
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    SmallclueLsEntry *entries = NULL;
    size_t count = 0;
    size_t capacity = 0;
    int status = 0;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        const char *filename = dir->d_name;
        if (!show_all && filename[0] == '.') {
            continue;
        }

        char *full_path = join_path(path, filename);
        if (!full_path) {
            fprintf(stderr, "ls: %s/%s: %s\n", path, filename, strerror(ENOMEM));
            status = 1;
            break;
        }

        struct stat stat_buf;
        if (lstat(full_path, &stat_buf) == -1) {
            fprintf(stderr, "ls: %s: %s\n", full_path, strerror(errno));
            free(full_path);
            status = 1;
            continue;
        }

        char *name_copy = strdup(filename);
        if (!name_copy) {
            fprintf(stderr, "ls: %s/%s: %s\n", path, filename, strerror(ENOMEM));
            free(full_path);
            status = 1;
            break;
        }

        if (count == capacity) {
            size_t new_capacity = capacity ? capacity * 2 : 64;
            SmallclueLsEntry *new_entries = (SmallclueLsEntry *)realloc(entries,
                                                                      new_capacity * sizeof(SmallclueLsEntry));
            if (!new_entries) {
                fprintf(stderr, "ls: %s: %s\n", path, strerror(ENOMEM));
                free(name_copy);
                free(full_path);
                status = 1;
                break;
            }
            entries = new_entries;
            capacity = new_capacity;
        }

        entries[count].name = name_copy;
        entries[count].full_path = full_path;
        entries[count].stat_buf = stat_buf;
        ++count;
    }
    closedir(d);

    if (sort_by_time && count > 1) {
        qsort(entries, count, sizeof(entries[0]), compare_ls_entries_by_mtime);
    }

    if (long_format) {
        for (size_t i = 0; i < count; ++i) {
            status |= print_path_entry_with_stat(entries[i].full_path,
                                                 entries[i].name,
                                                 true,
                                                 human,
                                                 &entries[i].stat_buf,
                                                 color_mode,
                                                 classify);
        }
    } else {
        print_ls_columns(entries, count, color_mode, classify);
    }

    free_ls_entries(entries, count);
    return status ? 1 : 0;
}

static void smallcluePrintAppletList(FILE *out, const char *heading) {
    if (!out) {
        return;
    }
    if (heading && *heading) {
        fprintf(out, "%s\n", heading);
    }
    for (size_t i = 0; i < kSmallclueAppletCount; ++i) {
        const SmallclueApplet *applet = &kSmallclueApplets[i];
        fprintf(out, "  %-14s %s\n", applet->name, applet->description ? applet->description : "");
    }
}


static void print_usage(void) {
    fprintf(stderr, "This is smallclue. Usage:\n");
    fprintf(stderr, "  smallclue <applet> [arguments...]\n\n");
    fprintf(stderr, "Available applets:\n");
    smallcluePrintAppletList(stderr, NULL);
    fprintf(stderr, "\nYou can symlink applets to 'smallclue' or invoke them directly.\n");
}

static int smallclueEchoCommand(int argc, char **argv) {
    int print_newline = 1;
    int start_index = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        print_newline = 0;
        start_index = 2;
    }
    for (int i = start_index; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) {
            putchar(' ');
        }
    }
    if (print_newline) {
        putchar('\n');
    }
    return 0;
}

static bool smallclueLsValidateShortOptions(const char *arg,
                                            int *show_all,
                                            int *long_format,
                                            int *sort_by_time,
                                            int *list_dirs_only,
                                            int *human_sizes,
                                            int *classify) {
    if (!arg) {
        return true;
    }
    for (const char *cursor = arg; *cursor; ++cursor) {
        switch (*cursor) {
            case 'a':
                *show_all = 1;
                break;
            case 'l':
                *long_format = 1;
                break;
            case 't':
                *sort_by_time = 1;
                break;
            case 'h':
                *human_sizes = 1;
                break;
            case 'd':
                *list_dirs_only = 1;
                break;
            case 'F':
                *classify = 1;
                break;
            default:
                fprintf(stderr, "ls: invalid option -- '%c'\n", *cursor);
                return false;
        }
    }
    return true;
}

static bool smallclueLsAcceptColorValue(const char *option_name, const char *value) {
    if (!value) {
        return true;
    }
    if (*value == '\0') {
        fprintf(stderr, "ls: invalid argument '' for '%s'\n", option_name);
        return false;
    }
    if (strcasecmp(value, "auto") == 0 ||
        strcasecmp(value, "always") == 0 ||
        strcasecmp(value, "never") == 0 ||
        strcasecmp(value, "none") == 0) {
        return true;
    }
    fprintf(stderr, "ls: invalid argument '%s' for '%s'\n", value, option_name);
    return false;
}

static bool smallclueLsHandleLongOption(const char *arg) {
    if (!arg) {
        return true;
    }
    if (strcmp(arg, "--color") == 0) {
        return smallclueLsAcceptColorValue("--color", NULL);
    }
    if (strcmp(arg, "--colour") == 0) {
        return smallclueLsAcceptColorValue("--colour", NULL);
    }
    if (strncmp(arg, "--color=", 8) == 0) {
        return smallclueLsAcceptColorValue("--color", arg + 8);
    }
    if (strncmp(arg, "--colour=", 9) == 0) {
        return smallclueLsAcceptColorValue("--colour", arg + 9);
    }
    fprintf(stderr, "ls: unrecognized option '%s'\n", arg);
    return false;
}

static int smallclueLsCommand(int argc, char **argv) {
    int show_all = 0;
    int long_format = 0;
    int sort_by_time = 0;
    int list_dirs_only = 0;
    int human_sizes = 0;
    int classify = 0;
    int color_mode = 0; /* 0=auto, 1=always, -1=never */
    smallclueResetGetopt();

    int idx = 1;
    while (idx < argc) {
        const char *arg = argv[idx];
        if (!arg || arg[0] != '-' || arg[1] == '\0') {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            idx++;
            break;
        }
        if (arg[1] == '-') {
            if (!smallclueLsHandleLongOption(arg)) {
                return 1;
            }
            if (strcmp(arg, "--color") == 0 || strcmp(arg, "--colour") == 0) {
                color_mode = 1;
            } else if (strncmp(arg, "--color=", 8) == 0) {
                const char *val = arg + 8;
                if (strcasecmp(val, "always") == 0) color_mode = 1;
                else if (strcasecmp(val, "never") == 0 || strcasecmp(val, "none") == 0) color_mode = -1;
                else color_mode = 0;
            } else if (strncmp(arg, "--colour=", 9) == 0) {
                const char *val = arg + 9;
                if (strcasecmp(val, "always") == 0) color_mode = 1;
                else if (strcasecmp(val, "never") == 0 || strcasecmp(val, "none") == 0) color_mode = -1;
                else color_mode = 0;
            }
            idx++;
            continue;
        }
        if (!smallclueLsValidateShortOptions(arg + 1,
                                             &show_all,
                                             &long_format,
                                             &sort_by_time,
                                             &list_dirs_only,
                                             &human_sizes,
                                             &classify)) {
            return 1;
        }
        idx++;
    }

    if (color_mode == 0) {
        color_mode = pscalRuntimeStdoutIsInteractive() ? 1 : -1;
    }

    int status = 0;
    int paths_start = idx;
    if (paths_start >= argc) {
        if (list_dirs_only) {
            return print_path_entry(".", ".", long_format, human_sizes, color_mode, classify) ? 1 : 0;
        }
        return list_directory(".", show_all, long_format, sort_by_time, human_sizes, color_mode, classify);
    }

    int remaining = argc - paths_start;
    for (int i = paths_start; i < argc; ++i) {
        const char *path = argv[i];
        struct stat stat_buf;
        if (lstat(path, &stat_buf) == -1) {
            fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
            status = 1;
            continue;
        }
        bool is_dir = S_ISDIR(stat_buf.st_mode);
        if (is_dir && !list_dirs_only) {
            if (remaining > 1) {
                if (i > paths_start) {
                    putchar('\n');
                }
                printf("%s:\n", path);
            }
            status |= list_directory(path, show_all, long_format, sort_by_time, human_sizes, color_mode, classify);
        } else {
            status |= print_path_entry_with_stat(path, path, long_format, human_sizes, &stat_buf, color_mode, classify);
        }
    }
    return status ? 1 : 0;
}

static int smallcluePwdCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        perror("pwd");
        return 1;
    }
    puts(cwd);
    return 0;
}

#define SMALLCLUE_CHMOD_TARGET_USER 0x1u
#define SMALLCLUE_CHMOD_TARGET_GROUP 0x2u
#define SMALLCLUE_CHMOD_TARGET_OTHER 0x4u
#define SMALLCLUE_CHMOD_TARGET_ALL (SMALLCLUE_CHMOD_TARGET_USER | SMALLCLUE_CHMOD_TARGET_GROUP | SMALLCLUE_CHMOD_TARGET_OTHER)

#define SMALLCLUE_CHMOD_PERM_READ 0x1u
#define SMALLCLUE_CHMOD_PERM_WRITE 0x2u
#define SMALLCLUE_CHMOD_PERM_EXEC 0x4u
#define SMALLCLUE_CHMOD_PERM_ALL (SMALLCLUE_CHMOD_PERM_READ | SMALLCLUE_CHMOD_PERM_WRITE | SMALLCLUE_CHMOD_PERM_EXEC)

typedef struct {
    unsigned targets;
    unsigned perms;
    char op;
} SmallclueChmodOp;

typedef struct {
    SmallclueChmodOp ops[16];
    size_t count;
} SmallclueChmodSpec;

static bool smallclueChmodParseOctal(const char *spec, mode_t *out_mode) {
    if (!spec || !out_mode) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long value = strtol(spec, &end, 8);
    if (errno != 0 || !end || *end != '\0' || value < 0 || value > 07777) {
        return false;
    }
    *out_mode = (mode_t)value;
    return true;
}

static bool smallclueChmodParseSymbolic(const char *spec, SmallclueChmodSpec *out_spec) {
    if (!spec || !out_spec) {
        return false;
    }
    out_spec->count = 0;
    const char *cursor = spec;
    while (*cursor) {
        if (out_spec->count >= sizeof(out_spec->ops) / sizeof(out_spec->ops[0])) {
            return false;
        }
        unsigned targets = 0;
        bool saw_target = false;
        while (*cursor == 'u' || *cursor == 'g' || *cursor == 'o' || *cursor == 'a') {
            saw_target = true;
            if (*cursor == 'u') targets |= SMALLCLUE_CHMOD_TARGET_USER;
            else if (*cursor == 'g') targets |= SMALLCLUE_CHMOD_TARGET_GROUP;
            else if (*cursor == 'o') targets |= SMALLCLUE_CHMOD_TARGET_OTHER;
            else if (*cursor == 'a') targets |= SMALLCLUE_CHMOD_TARGET_ALL;
            cursor++;
        }
        if (!saw_target) {
            targets = SMALLCLUE_CHMOD_TARGET_ALL;
        }
        char op = *cursor;
        if (op != '+' && op != '-' && op != '=') {
            return false;
        }
        cursor++;
        unsigned perms = 0;
        while (*cursor == 'r' || *cursor == 'w' || *cursor == 'x') {
            if (*cursor == 'r') perms |= SMALLCLUE_CHMOD_PERM_READ;
            else if (*cursor == 'w') perms |= SMALLCLUE_CHMOD_PERM_WRITE;
            else if (*cursor == 'x') perms |= SMALLCLUE_CHMOD_PERM_EXEC;
            cursor++;
        }
        if (op != '=' && perms == 0) {
            return false;
        }
        SmallclueChmodOp *entry = &out_spec->ops[out_spec->count++];
        entry->targets = targets;
        entry->perms = perms;
        entry->op = op;
        if (*cursor == ',') {
            cursor++;
            continue;
        } else if (*cursor == '\0') {
            break;
        } else {
            return false;
        }
    }
    return out_spec->count > 0;
}

static mode_t smallclueChmodMaskForTargets(unsigned targets, unsigned perms) {
    mode_t mask = 0;
    if (perms & SMALLCLUE_CHMOD_PERM_READ) {
        if (targets & SMALLCLUE_CHMOD_TARGET_USER) mask |= S_IRUSR;
        if (targets & SMALLCLUE_CHMOD_TARGET_GROUP) mask |= S_IRGRP;
        if (targets & SMALLCLUE_CHMOD_TARGET_OTHER) mask |= S_IROTH;
    }
    if (perms & SMALLCLUE_CHMOD_PERM_WRITE) {
        if (targets & SMALLCLUE_CHMOD_TARGET_USER) mask |= S_IWUSR;
        if (targets & SMALLCLUE_CHMOD_TARGET_GROUP) mask |= S_IWGRP;
        if (targets & SMALLCLUE_CHMOD_TARGET_OTHER) mask |= S_IWOTH;
    }
    if (perms & SMALLCLUE_CHMOD_PERM_EXEC) {
        if (targets & SMALLCLUE_CHMOD_TARGET_USER) mask |= S_IXUSR;
        if (targets & SMALLCLUE_CHMOD_TARGET_GROUP) mask |= S_IXGRP;
        if (targets & SMALLCLUE_CHMOD_TARGET_OTHER) mask |= S_IXOTH;
    }
    return mask;
}

static mode_t smallclueChmodApplySpec(mode_t current, const SmallclueChmodSpec *spec) {
    mode_t result = current;
    if (!spec) {
        return result;
    }
    for (size_t i = 0; i < spec->count; ++i) {
        const SmallclueChmodOp *op = &spec->ops[i];
        mode_t mask = smallclueChmodMaskForTargets(op->targets, op->perms);
        switch (op->op) {
            case '+':
                result |= mask;
                break;
            case '-':
                result &= ~mask;
                break;
            case '=': {
                mode_t clearMask = smallclueChmodMaskForTargets(op->targets, SMALLCLUE_CHMOD_PERM_ALL);
                result &= ~clearMask;
                result |= mask;
                break;
            }
            default:
                break;
        }
    }
    return result;
}

static int smallclueChmodApplySymbolic(const SmallclueChmodSpec *spec, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        return -1;
    }
    mode_t desired = smallclueChmodApplySpec(st.st_mode, spec);
    if (chmod(path, desired) != 0) {
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int smallclueChmodCommand(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: chmod mode file...\n");
        return 1;
    }
    mode_t octalMode = 0;
    SmallclueChmodSpec symbolicSpec;
    bool useOctal = smallclueChmodParseOctal(argv[1], &octalMode);
    bool useSymbolic = false;
    if (!useOctal) {
        useSymbolic = smallclueChmodParseSymbolic(argv[1], &symbolicSpec);
    }
    if (!useOctal && !useSymbolic) {
        fprintf(stderr, "chmod: invalid mode: %s\n", argv[1]);
        return 1;
    }
    int status = 0;
    for (int i = 2; i < argc; ++i) {
        if (useOctal) {
            if (chmod(argv[i], octalMode) != 0) {
                fprintf(stderr, "chmod: %s: %s\n", argv[i], strerror(errno));
                status = 1;
            }
        } else {
            if (smallclueChmodApplySymbolic(&symbolicSpec, argv[i]) != 0) {
                status = 1;
            }
        }
    }
    return status;
}

static int smallclueTrueCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 0;
}

static int smallclueFalseCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    return 1;
}

#if defined(PSCAL_TARGET_IOS)

typedef struct {
    const char *name;
    const char *path;
} SmallclueLicense;

static const SmallclueLicense kSmallclueLicenses[] = {
    {"PSCAL", "/home/Docs/Licenses/pscal.txt"},
    {"OpenSSH", "/home/Docs/Licenses/openssh.txt"},
    {"curl", "/home/Docs/Licenses/curl.txt"},
    {"OpenSSL", "/home/Docs/Licenses/openssl.txt"},
    {"SDL2", "/home/Docs/Licenses/sdl2.txt"},
    {"Nextvi", "/home/Docs/Licenses/nextvi.txt"},
    {"yyjson", "/home/Docs/Licenses/yyjson.txt"},
};

static size_t smallclueLicensesCount(void) {
    return sizeof(kSmallclueLicenses) / sizeof(kSmallclueLicenses[0]);
}

static void smallclueLicensesRenderMenu(size_t selected) {
    printf("\033[2J\033[H");
    printf("PSCAL & Third-Party Licenses\n");
    printf("Use arrows to navigate, Enter to view, q to quit.\n\n");
    size_t total = smallclueLicensesCount();
    for (size_t i = 0; i < total; ++i) {
        const char *marker = (i == selected) ? ">" : " ";
        printf("%s %s\n", marker, kSmallclueLicenses[i].name);
    }
    fflush(stdout);
}

static int smallclueLicensesCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    size_t total = smallclueLicensesCount();
    if (total == 0) {
        fprintf(stderr, "licenses: no entries available\n");
        return 1;
    }
    size_t selected = 0;
    bool running = true;
    while (running) {
        smallclueLicensesRenderMenu(selected);
        int key = pager_read_key();
        switch (key) {
            case PAGER_KEY_ARROW_UP:
                selected = (selected == 0) ? (total - 1) : (selected - 1);
                break;
            case PAGER_KEY_ARROW_DOWN:
                selected = (selected + 1) % total;
                break;
            case '\r':
            case '\n': {
                const SmallclueLicense *entry = &kSmallclueLicenses[selected];
                FILE *probe = fopen(entry->path, "r");
                if (!probe) {
                    fprintf(stderr, "licenses: %s: %s\n", entry->path, strerror(errno));
                    break;
                }
                fclose(probe);
                char *argv_lic[3];
                argv_lic[0] = "less";
                argv_lic[1] = (char *)entry->path;
                argv_lic[2] = NULL;
                smallclueLicensesRenderMenu(selected);
                (void)smallcluePagerCommand(2, argv_lic);
                break;
            }
            case 'q':
            case 'Q':
            case 0x1b:
                running = false;
                break;
            default:
                break;
        }
    }
    printf("\033[2J\033[H");
    fflush(stdout);
    return 0;
}

static int smallclueHelpCommand(int argc, char **argv) {
    int status = 0;
    char *buffer = NULL;
    size_t buflen = 0;
    FILE *mem = open_memstream(&buffer, &buflen);
    if (!mem) {
        fprintf(stderr, "smallclue-help: unable to allocate buffer\n");
        return 1;
    }

    if (argc <= 1) {
        smallcluePrintAppletList(mem, "Available smallclue applets:");
    } else {
        for (int i = 1; i < argc; ++i) {
            const char *target = argv[i];
            const SmallclueApplet *applet = smallclueFindApplet(target);
            if (!applet) {
                fprintf(mem, "smallclue-help: '%s' not found\n", target);
                status = 1;
                continue;
            }
            const char *usage = smallclueLookupHelp(applet->name);
            fprintf(mem, "%s - %s\n", applet->name, applet->description ? applet->description : "");
            if (usage) {
                fprintf(mem, "Usage:\n%s\n", usage);
            } else {
                fprintf(mem, "(No detailed help available for this applet)\n\n");
            }
        }
    }
    fflush(mem);
    fclose(mem);

    if (!buffer) {
        return status;
    }

    bool interactive_out = pscalRuntimeStdoutIsInteractive();
    if (interactive_out) {
        FILE *r = fmemopen(buffer, buflen, "r");
        if (!r) {
            interactive_out = false;
        } else {
            int rows = 0;
            int cols = 0;
            smallclueGetTerminalSize(&rows, &cols);
            if (rows <= 0) {
                rows = INT_MAX;
            }
            int line_count = 0;
            for (char *p = buffer; *p; ++p) {
                if (*p == '\n') line_count++;
            }
            if (line_count >= rows) {
                pager_file("smallclue-help", "(internal)", r);
            } else {
                // Print directly if it fits on one screen
                fwrite(buffer, 1, buflen, stdout);
            }
            fclose(r);
        }
    } else {
        fwrite(buffer, 1, buflen, stdout);
    }
    free(buffer);
    return status;
}
#endif

#if defined(SMALLCLUE_WITH_EXSH)
static int smallclueShCommand(int argc, char **argv) {
    return exsh_main(argc, argv);
}
#endif

static int64_t smallclueUptimeSeconds(void) {
#if defined(__APPLE__)
    struct timeval boottv;
    size_t len = sizeof(boottv);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottv, &len, NULL, 0) == 0 && boottv.tv_sec > 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        time_t secs = now.tv_sec - boottv.tv_sec;
        if (secs < 0) {
            secs = 0;
        }
        return (int64_t)secs;
    }
#endif
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (int64_t)ts.tv_sec;
    }
    return -1;
}

static int smallclueUptimeCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int64_t seconds = smallclueUptimeSeconds();
    if (seconds < 0) {
        fprintf(stderr, "uptime: unavailable\n");
        return 1;
    }
    int days = (int)(seconds / 86400);
    seconds %= 86400;
    int hours = (int)(seconds / 3600);
    seconds %= 3600;
    int minutes = (int)(seconds / 60);
    int secs = (int)(seconds % 60);
    if (days > 0) {
        printf("up %d day%s, %02d:%02d:%02d\n", days, days == 1 ? "" : "s", hours, minutes, secs);
    } else {
        printf("up %02d:%02d:%02d\n", hours, minutes, secs);
    }
    return 0;
}

static int smallclueWatchRunApplet(const SmallclueApplet *applet, int argc, char **argv) {
    if (!applet) {
        fprintf(stderr, "watch: %s: command not found\n", argv[0]);
        return 127;
    }
    return smallclueDispatchApplet(applet, argc, argv);
}

static int smallclueWatchCommand(int argc, char **argv) {
    smallclueResetGetopt();
    smallclueClearPendingSignals();
    double interval = 2.0;
    int idx = 1;
    while (idx < argc) {
        const char *arg = argv[idx];
        if (!arg || arg[0] != '-') {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            idx++;
            break;
        }
        if (strcmp(arg, "-n") == 0) {
            if (idx + 1 >= argc) {
                fprintf(stderr, "watch: option requires an argument -- n\n");
                return 1;
            }
            char *endptr = NULL;
            interval = strtod(argv[idx + 1], &endptr);
            if (!endptr || *endptr != '\0' || interval <= 0.0) {
                fprintf(stderr, "watch: invalid interval '%s'\n", argv[idx + 1]);
                return 1;
            }
            idx += 2;
            continue;
        }
        fprintf(stderr, "watch: unsupported option '%s'\n", arg);
        return 1;
    }

    int cmd_argc = argc - idx;
    if (cmd_argc < 1) {
        fprintf(stderr, "watch: command required\n");
        return 1;
    }

    const SmallclueApplet *applet = smallclueFindApplet(argv[idx]);

    char *cmdline = NULL;
    size_t cmdlen = 0;
    for (int i = idx; i < argc; ++i) {
        size_t part = strlen(argv[i]);
        char *next = (char *)realloc(cmdline, cmdlen + part + 2);
        if (!next) {
            free(cmdline);
            fprintf(stderr, "watch: out of memory\n");
            return 1;
        }
        cmdline = next;
        memcpy(cmdline + cmdlen, argv[i], part);
        cmdlen += part;
        cmdline[cmdlen++] = (i + 1 < argc) ? ' ' : '\0';
    }

    int status = 0;
    while (1) {
        int abort_status = 0;
        if (smallclueShouldAbort(&abort_status)) {
            status = abort_status;
            break;
        }
        printf("\033[H\033[J");
        printf("Every %.2fs: %s\n\n", interval, cmdline ? cmdline : argv[idx]);
        fflush(stdout);
        status = smallclueWatchRunApplet(applet, cmd_argc, &argv[idx]);
        fflush(stdout);
        struct timespec ts;
        ts.tv_sec = (time_t)interval;
        ts.tv_nsec = (long)((interval - (double)ts.tv_sec) * 1e9);
        if (ts.tv_nsec < 0) {
            ts.tv_nsec = 0;
        }
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        if (smallclueShouldAbort(&abort_status)) {
            status = abort_status;
            goto watch_done;
        }
        continue;
    }
}

watch_done:
    free(cmdline);
    return status;
}

static int smallclueSleepCommand(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sleep seconds\n");
        return 1;
    }
    errno = 0;
    char *end = NULL;
    double seconds = strtod(argv[1], &end);
    if (errno != 0 || !end || *end != '\0' || seconds < 0) {
        fprintf(stderr, "sleep: invalid time interval '%s'\n", argv[1]);
        return 1;
    }
    struct timespec req;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    if (req.tv_nsec < 0) {
        req.tv_nsec = 0;
    }
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {
        continue;
    }
    return 0;
}

static int smallclueBasenameCommand(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "basename: missing operand\n");
        return 1;
    }
    char *path = strdup(argv[1]);
    if (!path) {
        perror("basename");
        return 1;
    }
    char *base = basename(path);
    if (base) {
        puts(base);
    }
    free(path);
    return base ? 0 : 1;
}

static int smallclueDirnameCommand(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "dirname: missing operand\n");
        return 1;
    }
    char *path = strdup(argv[1]);
    if (!path) {
        perror("dirname");
        return 1;
    }
    char *dir = dirname(path);
    if (dir) {
        puts(dir);
    }
    free(path);
    return dir ? 0 : 1;
}

static int smallclueTeeCommand(int argc, char **argv) {
    smallclueResetGetopt();
    int append = 0;
    int opt;
    while ((opt = getopt(argc, argv, "a")) != -1) {
        if (opt == 'a') {
            append = 1;
        } else {
            fprintf(stderr, "usage: tee [-a] [file...]\n");
            return 1;
        }
    }
    int file_count = argc - optind;
    FILE **files = NULL;
    if (file_count > 0) {
        files = (FILE **)calloc((size_t)file_count, sizeof(FILE *));
        if (!files) {
            perror("tee");
            return 1;
        }
        for (int i = 0; i < file_count; ++i) {
            const char *mode = append ? "ab" : "wb";
            files[i] = fopen(argv[optind + i], mode);
            if (!files[i]) {
                fprintf(stderr, "tee: %s: %s\n", argv[optind + i], strerror(errno));
            }
        }
    }
    int status = 0;
    char buffer[4096];
    size_t nread;
    while ((nread = fread(buffer, 1, sizeof(buffer), stdin)) > 0) {
        if (fwrite(buffer, 1, nread, stdout) != nread) {
            perror("tee");
            status = 1;
            break;
        }
        for (int i = 0; i < file_count; ++i) {
            if (!files[i]) {
                continue;
            }
            if (fwrite(buffer, 1, nread, files[i]) != nread) {
                fprintf(stderr, "tee: %s: %s\n", argv[optind + i], strerror(errno));
                fclose(files[i]);
                files[i] = NULL;
                status = 1;
            }
        }
    }
    if (ferror(stdin)) {
        perror("tee");
        status = 1;
    }
    if (files) {
        for (int i = 0; i < file_count; ++i) {
            if (files[i]) {
                fclose(files[i]);
            }
        }
        free(files);
    }
    return status;
}

static int smallclueElvisCommand(int argc, char **argv) {
    return smallclueRunElvis(argc, argv);
}
static int smallclueSshCommand(int argc, char **argv) {
    return smallclueRunSsh(argc, argv);
}
static int smallclueScpCommand(int argc, char **argv) {
    return smallclueRunScp(argc, argv);
}
static int smallclueSftpCommand(int argc, char **argv) {
    return smallclueRunSftp(argc, argv);
}
static int smallclueSshKeygenCommand(int argc, char **argv) {
    return smallclueRunSshKeygen(argc, argv);
}
#if SMALLCLUE_HAS_IFADDRS
static void smallclueIpAddrUsage(void) {
    fputs("usage: ipaddr [-4|-6] [-a]\n", stderr);
}

static bool smallclueShouldSkipInterface(const struct ifaddrs *ifa, int family, bool show_all) {
    if (show_all || !ifa) {
        return false;
    }
    if (ifa->ifa_flags & IFF_LOOPBACK) {
        return true;
    }
#if defined(AF_INET6)
    if (family == AF_INET6 && ifa->ifa_addr) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ifa->ifa_addr;
        if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) || IN6_IS_ADDR_MULTICAST(&sin6->sin6_addr)) {
            return true;
        }
    }
#endif
    return false;
}

static int smallclueIpAddrCommand(int argc, char **argv) {
    bool request_v4 = false;
    bool request_v6 = false;
    bool show_all = false;
    smallclueResetGetopt();
    int opt;
    while ((opt = getopt(argc, argv, "46ah")) != -1) {
        switch (opt) {
            case '4':
                request_v4 = true;
                break;
            case '6':
                request_v6 = true;
                break;
            case 'a':
                show_all = true;
                break;
            case 'h':
            default:
                smallclueIpAddrUsage();
                return 1;
        }
    }
    if (optind != argc) {
        smallclueIpAddrUsage();
        return 1;
    }
    bool show_v4 = request_v4 || (!request_v4 && !request_v6);
    bool show_v6 = request_v6 || (!request_v4 && !request_v6);

    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) {
        fprintf(stderr, "ipaddr: getifaddrs failed: %s\n", strerror(errno));
        return 1;
    }

    bool printed = false;
    for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        int family = ifa->ifa_addr->sa_family;
        if (family == AF_INET) {
            if (!show_v4) continue;
        }
#if defined(AF_INET6)
        else if (family == AF_INET6) {
            if (!show_v6) continue;
        }
#endif
        else {
            continue;
        }
        if (smallclueShouldSkipInterface(ifa, family, show_all)) {
            continue;
        }

        char host[NI_MAXHOST];
        socklen_t addrlen = (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
        int flags = NI_NUMERICHOST;
        if (getnameinfo(ifa->ifa_addr, addrlen, host, sizeof(host), NULL, 0, flags) != 0) {
            continue;
        }
        printf("%-12s %-4s %s\n",
            ifa->ifa_name ? ifa->ifa_name : "(unknown)",
            (family == AF_INET) ? "IPv4" : "IPv6",
            host);
        printed = true;
    }
    freeifaddrs(ifaddr);
    if (!printed) {
        fprintf(stderr, "ipaddr: no matching interfaces\n");
        return 1;
    }
    return 0;
}
#endif

typedef struct {
    unsigned long long total_bytes;
    unsigned long long free_bytes;
    unsigned long long avail_bytes;
#if defined(SMALLCLUE_HAVE_STATFS) && defined(MNAMELEN)
    char mount_point[MNAMELEN];
#else
    char mount_point[PATH_MAX];
#endif
} SmallclueDfStats;

static bool smallclueDfQuery(const char *path, SmallclueDfStats *out) {
    if (!path || !*path || !out) {
        errno = EINVAL;
        return false;
    }
    memset(out, 0, sizeof(*out));
#if defined(SMALLCLUE_HAVE_STATVFS)
    struct statvfs st;
    if (statvfs(path, &st) != 0) {
        return false;
    }
    unsigned long long block_size = st.f_frsize ? st.f_frsize : st.f_bsize;
#elif defined(SMALLCLUE_HAVE_STATFS)
    struct statfs st;
    if (statfs(path, &st) != 0) {
        return false;
    }
    unsigned long long block_size = st.f_bsize ? st.f_bsize : st.f_iosize;
#else
#error "Either statvfs or statfs must be available for df command"
#endif
    if (block_size == 0) {
        errno = EINVAL;
        return false;
    }
#if defined(SMALLCLUE_HAVE_STATVFS)
    unsigned long long total_blocks = st.f_blocks;
    unsigned long long free_blocks = st.f_bfree;
    unsigned long long avail_blocks = st.f_bavail;
#else
    unsigned long long total_blocks = st.f_blocks;
    unsigned long long free_blocks = st.f_bfree;
    unsigned long long avail_blocks = st.f_bavail;
#endif
    out->total_bytes = total_blocks * block_size;
    out->free_bytes = free_blocks * block_size;
    out->avail_bytes = avail_blocks * block_size;
#if defined(SMALLCLUE_HAVE_STATFS) && defined(MNAMELEN)
    if (st.f_mntonname[0]) {
        strncpy(out->mount_point, st.f_mntonname, sizeof(out->mount_point) - 1);
        out->mount_point[sizeof(out->mount_point) - 1] = '\0';
    }
#endif
    if (out->mount_point[0] == '\0') {
        const char *label = path;
        char resolved[PATH_MAX];
        if (realpath(path, resolved)) {
            label = resolved;
        }
        strncpy(out->mount_point, label, sizeof(out->mount_point) - 1);
        out->mount_point[sizeof(out->mount_point) - 1] = '\0';
    }
    return true;
}

static void smallclueDfFormatSize(char *buf, size_t bufsize,
                                  unsigned long long bytes, bool human) {
    if (!buf || bufsize == 0) {
        return;
    }
    if (!human) {
        unsigned long long blocks = (bytes + 1023ULL) / 1024ULL;
        snprintf(buf, bufsize, "%llu", blocks);
        return;
    }
    static const char *suffixes[] = {"B", "K", "M", "G", "T", "P"};
    size_t idx = 0;
    long double value = (long double)bytes;
    while (value >= 1024.0L && idx + 1 < sizeof(suffixes) / sizeof(suffixes[0])) {
        value /= 1024.0L;
        idx++;
    }
    if (idx == 0) {
        /* Bytes should not show fractional values. */
        snprintf(buf, bufsize, "%.0Lf%s", value, suffixes[idx]);
    } else if (value >= 100.0L) {
        snprintf(buf, bufsize, "%.0Lf%s", value, suffixes[idx]);
    } else if (value >= 10.0L) {
        snprintf(buf, bufsize, "%.1Lf%s", value, suffixes[idx]);
    } else {
        snprintf(buf, bufsize, "%.2Lf%s", value, suffixes[idx]);
    }
}

static void smallclueDfPrintHeader(bool human) {
    printf("%-24s %12s %12s %12s %6s %s\n",
           "Filesystem",
           human ? "Size" : "1K-blocks",
           human ? "Used" : "Used",
           human ? "Avail" : "Avail",
           "Use%",
           "Mounted on");
}

static int smallclueDfCommand(int argc, char **argv) {
    const char *usage = "usage: df [-h] [path ...]\n";
    bool human = false;
    smallclueResetGetopt();
    int opt;
    while ((opt = getopt(argc, argv, "h")) != -1) {
        switch (opt) {
            case 'h':
                human = true;
                break;
            default:
                fputs(usage, stderr);
                return 1;
        }
    }
    int path_start = optind;
    int path_count = (optind < argc) ? (argc - optind) : 0;
    smallclueDfPrintHeader(human);
    int status = 0;
    for (int i = 0; i < (path_count > 0 ? path_count : 1); ++i) {
        const char *path = (path_count > 0) ? argv[path_start + i] : ".";
        SmallclueDfStats stats;
        if (!smallclueDfQuery(path, &stats)) {
            fprintf(stderr, "df: %s: %s\n", path ? path : "(null)", strerror(errno));
            status = 1;
            continue;
        }
        unsigned long long used_bytes = (stats.total_bytes > stats.free_bytes)
                                            ? stats.total_bytes - stats.free_bytes
                                            : 0;
        unsigned long long avail_bytes = stats.avail_bytes;
        long double denom = (long double)used_bytes + (long double)avail_bytes;
        long double percent = (denom > 0.0L) ? (long double)used_bytes / denom * 100.0L : 0.0L;
        char total_buf[32];
        char used_buf[32];
        char avail_buf[32];
        smallclueDfFormatSize(total_buf, sizeof total_buf, stats.total_bytes, human);
        smallclueDfFormatSize(used_buf, sizeof used_buf, used_bytes, human);
        smallclueDfFormatSize(avail_buf, sizeof avail_buf, avail_bytes, human);
        printf("%-24s %12s %12s %12s %5.0Lf%% %s\n",
               stats.mount_point[0] ? stats.mount_point : (path ? path : ""),
               total_buf,
               used_buf,
               avail_buf,
               percent,
               path ? path : stats.mount_point);
    }
    return status;
}
static int smallcluePingAttempt(const struct sockaddr *addr, socklen_t addrlen, int family, int timeout_ms, double *out_ms, int probe_port) {
    int sock = socket(family, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return -1;
    }
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int rc = -1;
    // If a specific port was provided, override it in the sockaddr.
    if (probe_port > 0) {
        if (addr->sa_family == AF_INET && addrlen >= sizeof(struct sockaddr_in)) {
            struct sockaddr_in tmp;
            memcpy(&tmp, addr, sizeof(tmp));
            tmp.sin_port = htons((uint16_t)probe_port);
            rc = connect(sock, (struct sockaddr *)&tmp, sizeof(tmp));
        } else if (addr->sa_family == AF_INET6 && addrlen >= sizeof(struct sockaddr_in6)) {
            struct sockaddr_in6 tmp6;
            memcpy(&tmp6, addr, sizeof(tmp6));
            tmp6.sin6_port = htons((uint16_t)probe_port);
            rc = connect(sock, (struct sockaddr *)&tmp6, sizeof(tmp6));
        } else {
            rc = connect(sock, addr, addrlen);
        }
    } else {
        rc = connect(sock, addr, addrlen);
    }
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rc = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (rc <= 0) {
            close(sock);
            errno = (rc == 0) ? ETIMEDOUT : errno;
            return -1;
        }
        int so_error = 0;
        socklen_t slen = sizeof(so_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &slen) < 0 || so_error != 0) {
            if (so_error != 0) {
                errno = so_error;
            }
            close(sock);
            return -1;
        }
    } else if (rc < 0) {
        close(sock);
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    close(sock);
    if (out_ms) {
        double start_ms = (double)start.tv_sec * 1000.0 + (double)start.tv_nsec / 1e6;
        double end_ms = (double)end.tv_sec * 1000.0 + (double)end.tv_nsec / 1e6;
        *out_ms = end_ms - start_ms;
    }
    return 0;
}

static int smallcluePingCommand(int argc, char **argv) {
    const char *usage = "usage: ping [-c count] [-p port] [-t timeout_ms] host\n";
    if (argc <= 1) {
        fputs(usage, stderr);
        return 1;
    }
    smallclueResetGetopt();
    int count = 4;
    int timeout_ms = 3000;
    int probe_port = 80;
    int opt;
    while ((opt = getopt(argc, argv, "c:p:t:")) != -1) {
        switch (opt) {
            case 'c':
                count = atoi(optarg);
                if (count <= 0) count = 4;
                break;
            case 'p':
                probe_port = atoi(optarg);
                if (probe_port <= 0 || probe_port > 65535) {
                    fprintf(stderr, "ping: invalid port '%s'\n", optarg);
                    return 1;
                }
                break;
            case 't':
                timeout_ms = atoi(optarg);
                if (timeout_ms <= 0) timeout_ms = 3000;
                break;
            default:
                fputs(usage, stderr);
                return 1;
        }
    }
    if (optind >= argc) {
        fputs(usage, stderr);
        return 1;
    }
    const char *host = argv[optind];
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo *res = NULL;
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", probe_port);
    int gai = pscalHostsGetAddrInfo(host, portbuf, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "ping: %s: %s\n", host, gai_strerror(gai));
        return 1;
    }
    struct addrinfo *selected = res;
    if (!selected) {
        fprintf(stderr, "ping: no addresses resolved for %s\n", host);
        freeaddrinfo(res);
        return 1;
    }
    struct sockaddr_storage target_addr;
    memcpy(&target_addr, selected->ai_addr, selected->ai_addrlen);
    socklen_t target_len = (socklen_t)selected->ai_addrlen;
    char addrbuf[NI_MAXHOST];
    if (getnameinfo((struct sockaddr *)&target_addr, target_len,
            addrbuf, sizeof(addrbuf), NULL, 0, NI_NUMERICHOST) != 0) {
        strncpy(addrbuf, "unknown", sizeof(addrbuf));
        addrbuf[sizeof(addrbuf) - 1] = '\0';
    }
    printf("PING %s (%s) TCP port %d, %d probes, timeout %d ms\n",
        host, addrbuf, probe_port, count, timeout_ms);
    int successes = 0;
    double min_ms = 0.0, max_ms = 0.0, total_ms = 0.0;
    for (int i = 0; i < count; ++i) {
        double elapsed = 0.0;
        int rc = smallcluePingAttempt((struct sockaddr *)&target_addr, target_len,
            selected->ai_family, timeout_ms, &elapsed, probe_port);
        if (rc == 0) {
            successes++;
            if (successes == 1 || elapsed < min_ms) min_ms = elapsed;
            if (elapsed > max_ms) max_ms = elapsed;
            total_ms += elapsed;
            printf("attempt %d: connected in %.2f ms\n", i + 1, elapsed);
        } else {
            printf("attempt %d: failed (%s)\n", i + 1, strerror(errno));
        }
        fflush(stdout);
        if (i + 1 < count) {
            usleep(500000);
        }
    }
    printf("--- %s ping statistics ---\n", host);
    printf("%d probes sent, %d successful, %d failed\n",
        count, successes, count - successes);
    if (successes > 0) {
        printf("round-trip min/avg/max = %.2f/%.2f/%.2f ms (TCP port %d)\n",
            min_ms, total_ms / successes, max_ms, probe_port);
    }
    pscalHostsFreeAddrInfo(res);
    return (successes > 0) ? 0 : 1;
}

#define TELNET_DEFAULT_PORT 23
#define TELNET_BUF_SIZE 4096

static int smallclueTelnetCommand(int argc, char **argv) {
    smallclueResetGetopt();
    int port = TELNET_DEFAULT_PORT;
    int opt;
    while ((opt = getopt(argc, argv, "p:")) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "telnet: invalid port '%s'\n", optarg);
                    return 1;
                }
                break;
            default:
                fprintf(stderr, "usage: telnet [-p PORT] HOST\n");
                return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: telnet [-p PORT] HOST\n");
        return 1;
    }
    const char *host = argv[optind];
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    struct addrinfo *res = NULL;
    int gai = pscalHostsGetAddrInfo(host, port_str, &hints, &res);
    if (gai != 0 || !res) {
        fprintf(stderr, "telnet: %s: %s\n", host, gai_strerror(gai));
        return 1;
    }

    int sock = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) {
            continue;
        }
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(sock);
        sock = -1;
    }
    pscalHostsFreeAddrInfo(res);
    if (sock < 0) {
        fprintf(stderr, "telnet: unable to connect to %s:%d\n", host, port);
        return 1;
    }

    int status = 0;
    bool running = true;
    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = (sock > STDIN_FILENO) ? sock : STDIN_FILENO;
        int rv = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rv < 0) {
            if (errno == EINTR) continue;
            status = 1;
            break;
        }
        if (FD_ISSET(sock, &rfds)) {
            char buf[TELNET_BUF_SIZE];
            ssize_t n = read(sock, buf, sizeof(buf));
            if (n <= 0) {
                running = false;
            } else {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        running = false;
                        status = 1;
                        break;
                    }
                    off += w;
                }
            }
        }
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char buf[TELNET_BUF_SIZE];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                shutdown(sock, SHUT_WR);
            } else {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(sock, buf + off, (size_t)(n - off));
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        running = false;
                        status = 1;
                        break;
                    }
                    off += w;
                }
            }
        }
    }
    close(sock);
    return status;
}

static int smallclueTracerouteCommand(int argc, char **argv) {
    // Simple, in-process traceroute using UDP probes and ICMP replies (IPv4 only).
    smallclueResetGetopt();
    int max_hops = 30;
    int probes = 3;
    int timeout_ms = 1500;
    int dest_port = 33434;

    int opt;
    while ((opt = getopt(argc, argv, "m:q:w:p:")) != -1) {
        switch (opt) {
            case 'm': max_hops = atoi(optarg); break;
            case 'q': probes = atoi(optarg); break;
            case 'w': timeout_ms = atoi(optarg); break;
            case 'p': dest_port = atoi(optarg); break;
            default:
                fprintf(stderr, "usage: traceroute [-m max_hops] [-q probes] [-w timeout_ms] [-p port] HOST\n");
                return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: traceroute [-m max_hops] [-q probes] [-w timeout_ms] [-p port] HOST\n");
        return 1;
    }
    const char *host = argv[optind];

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *res = NULL;
    char portbuf[16];
    snprintf(portbuf, sizeof(portbuf), "%d", dest_port);
    int gai = pscalHostsGetAddrInfo(host, portbuf, &hints, &res);
    if (gai != 0 || !res) {
        fprintf(stderr, "traceroute: %s: %s\n", host, gai_strerror(gai));
        return 1;
    }

    int recv_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (recv_sock < 0) {
        fprintf(stderr, "traceroute: unable to open ICMP socket: %s\n", strerror(errno));
        pscalHostsFreeAddrInfo(res);
        return 1;
    }

    int send_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (send_sock < 0) {
        fprintf(stderr, "traceroute: unable to open UDP socket: %s\n", strerror(errno));
        close(recv_sock);
        pscalHostsFreeAddrInfo(res);
        return 1;
    }

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(recv_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("traceroute to %s (%s), %d hops max\n", host,
           inet_ntoa(((struct sockaddr_in *)res->ai_addr)->sin_addr), max_hops);

    bool reached = false;
    for (int ttl = 1; ttl <= max_hops && !reached; ttl++) {
        if (setsockopt(send_sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            fprintf(stderr, "traceroute: setsockopt(IP_TTL) failed: %s\n", strerror(errno));
            break;
        }

        printf("%2d ", ttl);
        fflush(stdout);
        bool got_reply = false;

        for (int p = 0; p < probes; p++) {
            struct sockaddr_in dest;
            memcpy(&dest, res->ai_addr, sizeof(dest));
            dest.sin_port = htons((uint16_t)(dest_port + ttl + p));

            struct timeval start, end;
            gettimeofday(&start, NULL);
            sendto(send_sock, "", 0, 0, (struct sockaddr *)&dest, sizeof(dest));

            unsigned char buf[1500];
            struct sockaddr_in reply_addr;
            socklen_t rlen = sizeof(reply_addr);
            ssize_t n = recvfrom(recv_sock, buf, sizeof(buf), 0, (struct sockaddr *)&reply_addr, &rlen);
            if (n < (ssize_t)(sizeof(struct ip) + sizeof(struct icmp))) {
                printf(" *");
                fflush(stdout);
                continue;
            }

            gettimeofday(&end, NULL);
            double rtt = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_usec - start.tv_usec) / 1000.0;

            struct ip *ip_hdr = (struct ip *)buf;
            int ip_hdr_len = ip_hdr->ip_hl << 2;
            struct icmp *icmp_hdr = (struct icmp *)(buf + ip_hdr_len);

            printf(" %s  %.3f ms", inet_ntoa(reply_addr.sin_addr), rtt);
            got_reply = true;

            if (icmp_hdr->icmp_type == ICMP_UNREACH && icmp_hdr->icmp_code == ICMP_UNREACH_PORT) {
                reached = true;
            }
        }

        if (!got_reply) {
            printf(" *");
        }
        printf("\n");
    }

    close(send_sock);
    close(recv_sock);
    freeaddrinfo(res);
    return reached ? 0 : 1;
}

#if defined(PSCAL_TARGET_IOS)
static int smallclueDmesgCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (pscalRuntimeCopySessionLog) {
        char *snapshot = pscalRuntimeCopySessionLog();
        if (snapshot) {
            fputs(snapshot, stdout);
            size_t len = strlen(snapshot);
            if (len > 0 && snapshot[len - 1] != '\n') {
                fputc('\n', stdout);
            }
            free(snapshot);
            return 0;
        }
    }
    fprintf(stderr, "dmesg: session log unavailable\n");
    return 1;
}
#endif

static int smallclueCatCommand(int argc, char **argv) {
    int status = 0;
    if (argc <= 1) {
        return cat_file(NULL);
    }
    for (int i = 1; i < argc; ++i) {
        status |= cat_file(argv[i]);
    }
    return status ? 1 : 0;
}

static int smallcluePagerCommand(int argc, char **argv) {
    const char *cmd_name = pager_command_name(argv && argc > 0 ? argv[0] : NULL);
    int status = 0;
    if (argc <= 1) {
        if (pscalRuntimeStdinIsInteractive()) {
            fprintf(stderr, "%s: missing filename\n", cmd_name);
            return 1;
        }
        return pager_file(cmd_name, "(stdin)", stdin);
    }
    for (int i = 1; i < argc; ++i) {
        const char *path = argv[i];
        if (!path || strcmp(path, "-") == 0) {
            status |= pager_file(cmd_name, "(stdin)", stdin);
            continue;
        }
        FILE *fp = fopen(path, "r");
        if (!fp) {
            fprintf(stderr, "%s: %s: %s\n", cmd_name, path, strerror(errno));
            status = 1;
            continue;
        }
        status |= pager_file(cmd_name, path, fp);
        fclose(fp);
    }
    return status ? 1 : 0;
}

static int smallclueMarkdownCommand(int argc, char **argv) {
    smallclueResetGetopt();
    int list_only = 0;
    int interactive = 0;
    int opt;
    while ((opt = getopt(argc, argv, "il")) != -1) {
        switch (opt) {
            case 'i':
                interactive = 1;
                break;
            case 'l':
                list_only = 1;
                break;
            default:
                fprintf(stderr, "usage: md [-i | -l] [file ...]\n");
                return 1;
        }
    }
    if (interactive) {
        if (optind < argc) {
            fprintf(stderr, "md: -i cannot be combined with file arguments\n");
            return 1;
        }
        return markdownInteractiveSelectDocument();
    }
    if (list_only || optind >= argc) {
        return smallclueMarkdownListDocuments();
    }
    int status = 0;
    for (int i = optind; i < argc; ++i) {
        status |= smallclueMarkdownDisplayPath(argv[i]);
    }
    return status ? 1 : 0;
}

static int smallclueCurlCommand(int argc, char **argv) {
    smallclueResetGetopt();
    const char *output_path = NULL;
    int use_remote_name = 0;
    int opt;
    while ((opt = getopt(argc, argv, "o:O")) != -1) {
        switch (opt) {
            case 'o':
                output_path = optarg;
                break;
            case 'O':
                use_remote_name = 1;
                break;
            default:
                fprintf(stderr, "usage: curl [-o file | -O] url...\n");
                return 1;
        }
    }
    if (output_path && use_remote_name) {
        fprintf(stderr, "curl: -o and -O may not be used together\n");
        return 1;
    }
    if (optind >= argc) {
        fprintf(stderr, "curl: missing URL\n");
        return 1;
    }
    if (output_path && (argc - optind) != 1) {
        fprintf(stderr, "curl: -o is only supported with a single URL\n");
        return 1;
    }
    int status = 0;
    for (int i = optind; i < argc; ++i) {
        const char *url = argv[i];
        const char *destination = NULL;
        char derived[PATH_MAX];
        if (output_path) {
            destination = output_path;
        } else if (use_remote_name) {
            smallclueUrlSuggestFilename(url, derived, sizeof(derived));
            destination = derived;
        }
        status |= smallclueHttpFetch("curl", url, destination);
    }
    return status ? 1 : 0;
}

static int smallclueWgetCommand(int argc, char **argv) {
    smallclueResetGetopt();
    const char *output_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "O:")) != -1) {
        switch (opt) {
            case 'O':
                output_path = optarg;
                break;
            default:
                fprintf(stderr, "usage: wget [-O file] url...\n");
                return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "wget: missing URL\n");
        return 1;
    }
    if (output_path && (argc - optind) != 1) {
        fprintf(stderr, "wget: -O is only supported with a single URL\n");
        return 1;
    }
    int status = 0;
    for (int i = optind; i < argc; ++i) {
        const char *url = argv[i];
        const char *destination = output_path;
        char derived[PATH_MAX];
        if (!destination) {
            smallclueUrlSuggestFilename(url, derived, sizeof(derived));
            destination = derived;
        }
        int rc = smallclueHttpFetch("wget", url, destination);
        if (rc == 0) {
            printf("Saved %s -> %s\n", url, destination ? destination : "(stdout)");
        }
        status |= rc;
    }
    return status ? 1 : 0;
}

static int smallclueClearCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fputs("\x1b[2J\x1b[H", stdout);
    fflush(stdout);
    return 0;
}

static int smallclueDateCommand(int argc, char **argv) {
    int arg_index = 1;
    int use_utc = 0;
    const char *format = "%a %b %e %T %Z %Y";

    while (arg_index < argc && argv[arg_index] && argv[arg_index][0] == '-') {
        const char *opt = argv[arg_index];
        if (strcmp(opt, "-u") == 0 || strcmp(opt, "--utc") == 0 || strcmp(opt, "--universal") == 0) {
            use_utc = 1;
            arg_index++;
            continue;
        }
        if (strcmp(opt, "--") == 0) {
            arg_index++;
            break;
        }
        fprintf(stderr, "date: unsupported option '%s'\n", opt);
        return 1;
    }

    if (arg_index < argc) {
        const char *fmt = argv[arg_index];
        if (fmt && fmt[0] == '+') {
            format = fmt + 1;
            arg_index++;
        } else {
            fprintf(stderr, "date: invalid format specifier '%s'\n", fmt ? fmt : "(null)");
            return 1;
        }
    }

    if (arg_index < argc) {
        fprintf(stderr, "date: too many operands\n");
        return 1;
    }

    time_t now = time(NULL);
    if (now == (time_t)-1) {
        perror("date");
        return 1;
    }
    struct tm tm_buf;
    struct tm *tm_val = use_utc ? gmtime(&now) : localtime(&now);
    if (!tm_val) {
        perror("date");
        return 1;
    }
    tm_buf = *tm_val;
    char buffer[256];
    size_t len = strftime(buffer, sizeof(buffer), format, &tm_buf);
    if (len == 0) {
        fprintf(stderr, "date: failed to format date\n");
        return 1;
    }
    printf("%s\n", buffer);
    return 0;
}

static bool smallclueParseInt(const char *text, int min, int max, int *out_value) {
    if (!text || !*text) {
        return false;
    }
    char *endptr = NULL;
    long value = strtol(text, &endptr, 10);
    if (!endptr || *endptr != '\0') {
        return false;
    }
    if (value < min || value > max) {
        return false;
    }
    if (out_value) {
        *out_value = (int)value;
    }
    return true;
}

static bool smallclueIsLeapYear(int year) {
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
}

static int smallclueDaysInMonth(int month, int year) {
    static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 30;
    }
    if (month == 2 && smallclueIsLeapYear(year)) {
        return 29;
    }
    return days_per_month[month - 1];
}

static int smallclueFirstWeekdayOfMonth(int month, int year) {
    struct tm tm_buf;
    memset(&tm_buf, 0, sizeof(tm_buf));
    tm_buf.tm_year = year - 1900;
    tm_buf.tm_mon = month - 1;
    tm_buf.tm_mday = 1;
    tm_buf.tm_isdst = -1;
    if (mktime(&tm_buf) == (time_t)-1) {
        return 0;
    }
    return tm_buf.tm_wday; /* 0 = Sunday */
}

static int smallclueCalCommand(int argc, char **argv) {
    int month = 0;
    int year = 0;

    if (argc == 1) {
        time_t now = time(NULL);
        if (now == (time_t)-1) {
            perror("cal");
            return 1;
        }
        struct tm *tm_now = localtime(&now);
        if (!tm_now) {
            perror("cal");
            return 1;
        }
        month = tm_now->tm_mon + 1;
        year = tm_now->tm_year + 1900;
    } else if (argc == 3) {
        if (!smallclueParseInt(argv[1], 1, 12, &month) || !smallclueParseInt(argv[2], 1, 9999, &year)) {
            fprintf(stderr, "cal: usage: cal [month] [year]\n");
            return 1;
        }
    } else {
        fprintf(stderr, "cal: usage: cal [month] [year]\n");
        return 1;
    }

    struct tm display_tm;
    memset(&display_tm, 0, sizeof(display_tm));
    display_tm.tm_year = year - 1900;
    display_tm.tm_mon = month - 1;
    display_tm.tm_mday = 1;
    char header[64];
    if (strftime(header, sizeof(header), "%B %Y", &display_tm) == 0) {
        snprintf(header, sizeof(header), "Month %d", year);
    }

    printf("      %s\n", header);
    printf("Su Mo Tu We Th Fr Sa\n");

    int first_wday = smallclueFirstWeekdayOfMonth(month, year);
    int days = smallclueDaysInMonth(month, year);
    int current_wday = 0;

    for (current_wday = 0; current_wday < first_wday; ++current_wday) {
        fputs("   ", stdout);
    }

    for (int day = 1; day <= days; ++day) {
        printf("%2d", day);
        current_wday++;
        if (current_wday % 7 == 0) {
            putchar('\n');
        } else {
            putchar(' ');
        }
    }
    if (current_wday % 7 != 0) {
        putchar('\n');
    }
    return 0;
}

static const char *smallclueStrCaseStr(const char *haystack, const char *needle, int ignore_case) {
    if (!haystack || !needle || !*needle) {
        return haystack;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        for (; i < needle_len; ++i) {
            char hc = p[i];
            char nc = needle[i];
            if (!hc) {
                break;
            }
            if (ignore_case) {
                hc = (char)tolower((unsigned char)hc);
                nc = (char)tolower((unsigned char)nc);
            }
            if (hc != nc) {
                break;
            }
        }
        if (i == needle_len) {
            return p;
        }
    }
    return NULL;
}

static bool smallclueParseDashLineCount(const char *arg, long *value) {
    if (!arg || !value || arg[0] != '-' || arg[1] == '\0') {
        return false;
    }
    if (arg[1] == '-') {
        return false;
    }
    const char *p = arg + 1;
    while (*p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
        p++;
    }
    char *endptr = NULL;
    long parsed = strtol(arg + 1, &endptr, 10);
    if (!endptr || *endptr != '\0') {
        return false;
    }
    *value = parsed;
    return true;
}

static int smallclueHeadStream(FILE *fp, const char *label, long lines) {
    if (lines <= 0) {
        return 0;
    }
    char *line = NULL;
    size_t cap = 0;
    long remaining = lines;
    int status = 0;
    while (remaining > 0) {
        ssize_t len = getline(&line, &cap, fp);
        if (len < 0) {
            if (ferror(fp)) {
                fprintf(stderr, "head: %s: %s\n", label ? label : "(stdin)", strerror(errno));
                status = 1;
            }
            break;
        }
        fwrite(line, 1, (size_t)len, stdout);
        remaining--;
    }
    free(line);
    return status;
}

static int smallclueHeadCommand(int argc, char **argv) {
    long lines = 10;
    int index = 1;
    while (index < argc) {
        const char *arg = argv[index];
        if (!arg || arg[0] != '-') {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }
        if (strcmp(arg, "-n") == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "head: option requires an argument -- n\n");
                return 1;
            }
            char *endptr = NULL;
            lines = strtol(argv[index + 1], &endptr, 10);
            if (!endptr || *endptr != '\0') {
                fprintf(stderr, "head: invalid line count '%s'\n", argv[index + 1]);
                return 1;
            }
            index += 2;
            continue;
        }
        long dashLines = 0;
        if (smallclueParseDashLineCount(arg, &dashLines)) {
            lines = dashLines;
            index += 1;
            continue;
        }
        fprintf(stderr, "head: unsupported option '%s'\n", arg);
        return 1;
    }

    int status = 0;
    if (index >= argc) {
        status = smallclueHeadStream(stdin, "(stdin)", lines);
    } else {
        for (int i = index; i < argc; ++i) {
            const char *path = argv[i];
            FILE *fp = fopen(path, "r");
            if (!fp) {
                fprintf(stderr, "head: %s: %s\n", path, strerror(errno));
                status = 1;
                continue;
            }
            status |= smallclueHeadStream(fp, path, lines);
            fclose(fp);
        }
    }
    return status ? 1 : 0;
}

static int smallclueTailStream(FILE *fp, const char *label, long lines) {
    if (lines <= 0) {
        return 0;
    }
    char **ring = (char **)calloc((size_t)lines, sizeof(char *));
    if (!ring) {
        fprintf(stderr, "tail: %s: out of memory\n", label ? label : "(stdin)");
        return 1;
    }
    char *line = NULL;
    size_t cap = 0;
    long count = 0;
    int status = 0;
    while (1) {
        ssize_t len = getline(&line, &cap, fp);
        if (len < 0) {
            if (ferror(fp)) {
                fprintf(stderr, "tail: %s: %s\n", label ? label : "(stdin)", strerror(errno));
                status = 1;
            }
            break;
        }
        char *copy = (char *)malloc((size_t)len + 1);
        if (!copy) {
            fprintf(stderr, "tail: %s: out of memory\n", label ? label : "(stdin)");
            status = 1;
            break;
        }
        memcpy(copy, line, (size_t)len);
        copy[len] = '\0';
        long slot = count % lines;
        free(ring[slot]);
        ring[slot] = copy;
        count++;
    }
    if (status == 0) {
        long start = count > lines ? count - lines : 0;
        for (long i = start; i < count; ++i) {
            char *entry = ring[i % lines];
            if (entry) {
                fputs(entry, stdout);
            }
        }
    }
    free(line);
    for (long i = 0; i < lines; ++i) {
        free(ring[i]);
    }
    free(ring);
    return status;
}

static int smallclueTailFollow(FILE *fp, const char *label, long lines) {
    int status = smallclueTailStream(fp, label, lines);
    if (status != 0) {
        return status;
    }
    char *line = NULL;
    size_t cap = 0;
    while (1) {
        if (smallclueShouldAbort(&status)) {
            break;
        }
        ssize_t len = getline(&line, &cap, fp);
        if (len < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (feof(fp)) {
                clearerr(fp);
                usleep(200000); /* 200ms */
                continue;
            }
            fprintf(stderr, "tail: %s: %s\n", label ? label : "(stdin)", strerror(errno));
            free(line);
            return 1;
        }
        fwrite(line, 1, (size_t)len, stdout);
        fflush(stdout);
    }
    /* unreachable */
    free(line);
    return 0;
}

#if defined(PSCAL_TARGET_IOS)
static void smallclueLogPathExpansion(const char *label, const char *path) {
    if (!pscalRuntimeDebugLog) {
        return;
    }
    char expanded[PATH_MAX];
    const char *resolved = path;
    if (path && pathTruncateExpand(path, expanded, sizeof(expanded))) {
        resolved = expanded;
    }
    char logbuf[PATH_MAX * 2];
    snprintf(logbuf, sizeof(logbuf), "[smallclue][%s] path=%s resolved=%s",
             label ? label : "touch",
             path ? path : "(null)",
             resolved ? resolved : "(null)");
    pscalRuntimeDebugLog(logbuf);
}
#endif

static int smallclueTailCommand(int argc, char **argv) {
    smallclueClearPendingSignals();
    long lines = 10;
    bool follow = false;
    int index = 1;
    while (index < argc) {
        const char *arg = argv[index];
        if (!arg || arg[0] != '-') {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }
        if (strcmp(arg, "-f") == 0) {
            follow = true;
            index += 1;
            continue;
        }
        if (strcmp(arg, "-n") == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "tail: option requires an argument -- n\n");
                return 1;
            }
            char *endptr = NULL;
            lines = strtol(argv[index + 1], &endptr, 10);
            if (!endptr || *endptr != '\0') {
                fprintf(stderr, "tail: invalid line count '%s'\n", argv[index + 1]);
                return 1;
            }
            index += 2;
            continue;
        }
        long dashLines = 0;
        if (smallclueParseDashLineCount(arg, &dashLines)) {
            lines = dashLines;
            index += 1;
            continue;
        }
        fprintf(stderr, "tail: unsupported option '%s'\n", arg);
        return 1;
    }
    if (follow && (argc - index) > 1) {
        fprintf(stderr, "tail: -f currently supports a single input\n");
        return 1;
    }
    int status = 0;
    if (index >= argc) {
        status = follow ? smallclueTailFollow(stdin, "(stdin)", lines)
                        : smallclueTailStream(stdin, "(stdin)", lines);
    } else {
        for (int i = index; i < argc; ++i) {
            const char *path = argv[i];
            FILE *fp = fopen(path, "r");
            if (!fp) {
                fprintf(stderr, "tail: %s: %s\n", path, strerror(errno));
                status = 1;
                continue;
            }
            if (follow) {
                status |= smallclueTailFollow(fp, path, lines);
                fclose(fp);
                break;
            } else {
                status |= smallclueTailStream(fp, path, lines);
                fclose(fp);
            }
        }
    }
    return status ? 1 : 0;
}

static int smallclueTouchCommand(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "touch: missing file operand\n");
        return 1;
    }
    int status = 0;
    struct timeval times[2];
    if (gettimeofday(&times[0], NULL) != 0) {
        times[0].tv_sec = time(NULL);
        times[0].tv_usec = 0;
    }
    times[1] = times[0];
    for (int i = 1; i < argc; ++i) {
        const char *path = argv[i];
        if (!path || !*path) {
            fprintf(stderr, "touch: invalid path\n");
            status = 1;
            continue;
        }
        const char *target = path;
#if defined(PSCAL_TARGET_IOS)
        char expanded[PATH_MAX];
        if (pathTruncateExpand(path, expanded, sizeof(expanded))) {
            target = expanded;
        }
#endif
        int fd = openat(AT_FDCWD, target, O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: %s\n", target, strerror(errno));
#if defined(PSCAL_TARGET_IOS)
            smallclueLogPathExpansion("touch-open-failed", target);
#endif
            status = 1;
            continue;
        }
        if (futimes(fd, times) != 0) {
            fprintf(stderr, "touch: %s: %s\n", target, strerror(errno));
#if defined(PSCAL_TARGET_IOS)
            smallclueLogPathExpansion("touch-futimes-failed", target);
#endif
            status = 1;
        }
        close(fd);
    }
    return status ? 1 : 0;
}

static long smallclueParseLong(const char *text) {
    if (!text) {
        return -1;
    }
    char *endptr = NULL;
    long value = strtol(text, &endptr, 10);
    if (!endptr || *endptr != '\0') {
        return -1;
    }
    return value;
}

static void smallclueEmitTerminalReset(void) {
    fputs("\x1b" "c", stdout); // RIS: full reset
    fflush(stdout);
}

static void smallclueEmitTerminalSane(void) {
    fputs("\x1b[0m\x1b[?7h\x1b[?25h", stdout); // reset attributes, enable wrap & cursor
    fflush(stdout);
}

static void smallclueGetTerminalSize(int *rows, int *cols) {
    int r = -1;
    int c = -1;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        r = ws.ws_row;
        c = ws.ws_col;
    } else if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        r = ws.ws_row;
        c = ws.ws_col;
    }
    if (r <= 0) {
        const char *env = getenv("LINES");
        if (env) {
            long val = smallclueParseLong(env);
            if (val > 0) r = (int)val;
        }
    }
    if (c <= 0) {
        const char *env = getenv("COLUMNS");
        if (env) {
            long val = smallclueParseLong(env);
            if (val > 0) c = (int)val;
        }
    }
    if (rows) *rows = r;
    if (cols) *cols = c;
}

static void smallclueApplyWindowSize(int rows, int cols) {
    if (rows > 0 && cols > 0) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", rows);
        setenv("LINES", buffer, 1);
        snprintf(buffer, sizeof(buffer), "%d", cols);
        setenv("COLUMNS", buffer, 1);

        if (isatty(STDOUT_FILENO)) {
            struct winsize ws;
            ws.ws_row = (unsigned short)rows;
            ws.ws_col = (unsigned short)cols;
            ws.ws_xpixel = 0;
            ws.ws_ypixel = 0;
            ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws);
        }
        if (isatty(STDIN_FILENO)) {
            struct winsize ws;
            ws.ws_row = (unsigned short)rows;
            ws.ws_col = (unsigned short)cols;
            ws.ws_xpixel = 0;
            ws.ws_ypixel = 0;
            ioctl(STDIN_FILENO, TIOCSWINSZ, &ws);
        }
        printf("\x1b[8;%d;%dt", rows, cols);
        fflush(stdout);

#if defined(PSCAL_TARGET_IOS)
        // On iOS we drive the runtime through the bridge so the master PTY
        // and any downstream consumers see the updated geometry immediately.
        if (PSCALRuntimeUpdateWindowSize) {
            PSCALRuntimeUpdateWindowSize(cols, rows);
        }
#endif
    }
}

static const char *smallclueBaudLabel(speed_t speed) {
#define CASE_BAUD(val) case val: return #val
    switch (speed) {
    CASE_BAUD(B0);
    CASE_BAUD(B50);
    CASE_BAUD(B75);
    CASE_BAUD(B110);
    CASE_BAUD(B134);
    CASE_BAUD(B150);
    CASE_BAUD(B200);
    CASE_BAUD(B300);
    CASE_BAUD(B600);
    CASE_BAUD(B1200);
    CASE_BAUD(B1800);
    CASE_BAUD(B2400);
    CASE_BAUD(B4800);
    CASE_BAUD(B9600);
#ifdef B19200
    CASE_BAUD(B19200);
#endif
#ifdef B38400
    CASE_BAUD(B38400);
#endif
#ifdef B57600
    CASE_BAUD(B57600);
#endif
#ifdef B115200
    CASE_BAUD(B115200);
#endif
#ifdef B230400
    CASE_BAUD(B230400);
#endif
    default:
        break;
    }
    static char unknown[32];
    snprintf(unknown, sizeof(unknown), "%lu", (unsigned long)speed);
    return unknown;
#undef CASE_BAUD
}

static void smallclueDescribeControlChar(const char *label, cc_t value) {
    const char *repr = NULL;
    char buffer[8];
#ifdef _POSIX_VDISABLE
    if (value == _POSIX_VDISABLE) {
        repr = "undef";
    } else
#endif
    if (value == 0) {
        repr = "^@";
    } else if (value < 32) {
        buffer[0] = '^';
        buffer[1] = (char)('A' + value - 1);
        buffer[2] = '\0';
        repr = buffer;
    } else if (value == 127) {
        repr = "^?";
    } else if (isprint((unsigned char)value)) {
        buffer[0] = (char)value;
        buffer[1] = '\0';
        repr = buffer;
    } else {
        snprintf(buffer, sizeof(buffer), "%u", (unsigned)value);
        repr = buffer;
    }
    printf("%s = %s; ", label, repr);
}

static int smallclueSttyReport(void) {
    if (!pscalRuntimeStdinHasRealTTY()) {
        fprintf(stderr, "stty: stdin is not a tty (running in virtual terminal)\n");
        int rows = pscalRuntimeDetectWindowRows();
        int cols = pscalRuntimeDetectWindowCols();
        if (rows <= 0) rows = 24;
        if (cols <= 0) cols = 80;
        printf("speed ? baud; rows %d; columns %d;\n", rows, cols);
        return 0;
    }
    struct termios tio;
    if (tcgetattr(STDIN_FILENO, &tio) != 0) {
        perror("stty");
        return 1;
    }
    speed_t ospeed = cfgetospeed(&tio);
    struct winsize ws;
    int rows = -1;
    int cols = -1;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    if (rows <= 0) {
        rows = pscalRuntimeDetectWindowRows();
    }
    if (cols <= 0) {
        cols = pscalRuntimeDetectWindowCols();
    }
    if (rows <= 0) rows = 24;
    if (cols <= 0) cols = 80;
    printf("speed %s baud; rows %d; columns %d;\n",
        smallclueBaudLabel(ospeed), rows, cols);

#ifdef VINTR
    smallclueDescribeControlChar("intr", tio.c_cc[VINTR]);
#endif
#ifdef VQUIT
    smallclueDescribeControlChar("quit", tio.c_cc[VQUIT]);
#endif
#ifdef VERASE
    smallclueDescribeControlChar("erase", tio.c_cc[VERASE]);
#endif
#ifdef VKILL
    smallclueDescribeControlChar("kill", tio.c_cc[VKILL]);
#endif
#ifdef VEOF
    smallclueDescribeControlChar("eof", tio.c_cc[VEOF]);
#endif
#ifdef VSTART
    smallclueDescribeControlChar("start", tio.c_cc[VSTART]);
#endif
#ifdef VSTOP
    smallclueDescribeControlChar("stop", tio.c_cc[VSTOP]);
#endif
#ifdef VSUSP
    smallclueDescribeControlChar("susp", tio.c_cc[VSUSP]);
#endif
    printf("\n");
#ifdef VMIN
    printf("min = %u; ", (unsigned)tio.c_cc[VMIN]);
#endif
#ifdef VTIME
    printf("time = %u;", (unsigned)tio.c_cc[VTIME]);
#endif
    printf("\n");
    return 0;
}

static int smallclueSttyCommand(int argc, char **argv) {
    if (argc <= 1) {
        return smallclueSttyReport();
    }
    long rows = -1;
    long cols = -1;
    bool requestReset = false;
    bool requestSane = false;
    int index = 1;
    while (index < argc) {
        const char *arg = argv[index];
        if (strcmp(arg, "reset") == 0) {
            requestReset = true;
            index += 1;
            continue;
        }
        if (strcmp(arg, "sane") == 0) {
            requestSane = true;
            index += 1;
            continue;
        }
        if (strcmp(arg, "rows") == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "stty: missing value after 'rows'\n");
                return 1;
            }
            rows = smallclueParseLong(argv[index + 1]);
            if (rows <= 0) {
                fprintf(stderr, "stty: invalid rows value '%s'\n", argv[index + 1]);
                return 1;
            }
            index += 2;
            continue;
        }
        if (strcmp(arg, "cols") == 0 || strcmp(arg, "columns") == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "stty: missing value after '%s'\n", arg);
                return 1;
            }
            cols = smallclueParseLong(argv[index + 1]);
            if (cols <= 0) {
                fprintf(stderr, "stty: invalid columns value '%s'\n", argv[index + 1]);
                return 1;
            }
            index += 2;
            continue;
        }
        if (strcmp(arg, "size") == 0) {
            if (index + 2 >= argc) {
                fprintf(stderr, "stty: 'size' requires two numbers\n");
                return 1;
            }
            rows = smallclueParseLong(argv[index + 1]);
            cols = smallclueParseLong(argv[index + 2]);
            if (rows <= 0 || cols <= 0) {
                fprintf(stderr, "stty: invalid size values\n");
                return 1;
            }
            index += 3;
            continue;
        }
        fprintf(stderr, "stty: unsupported argument '%s'\n", arg);
        return 1;
    }

    if (requestReset) {
        smallclueEmitTerminalReset();
    }
    if (requestSane) {
        smallclueEmitTerminalSane();
    }

    if (rows <= 0 && cols <= 0) {
        if (requestReset || requestSane) {
            return 0;
        }
        fprintf(stderr, "Usage: stty rows <n> [cols <n>]\n");
        return 1;
    }

    if (rows <= 0) {
        const char *env = getenv("LINES");
        rows = env ? smallclueParseLong(env) : 24;
        if (rows <= 0) rows = 24;
    }
    if (cols <= 0) {
        const char *env = getenv("COLUMNS");
        cols = env ? smallclueParseLong(env) : 80;
        if (cols <= 0) cols = 80;
    }

    smallclueApplyWindowSize((int)rows, (int)cols);
    return 0;
}

static int smallclueResizeCommand(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        fprintf(stderr, "resize: does not accept arguments\n");
        return 1;
    }
    int rows = pscalRuntimeDetectWindowRows();
    int cols = pscalRuntimeDetectWindowCols();
    if (rows <= 0 || cols <= 0) {
        fprintf(stderr, "resize: unable to determine current window size\n");
        return 1;
    }
    smallclueApplyWindowSize(rows, cols);
    return 0;
}

static int smallclueSortCommand(int argc, char **argv) {
    int reverse = 0;
    int index = 1;
    while (index < argc) {
        const char *arg = argv[index];
        if (!arg || arg[0] != '-') {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }
        if (strcmp(arg, "-r") == 0) {
            reverse = 1;
            index++;
            continue;
        }
        fprintf(stderr, "sort: unsupported option '%s'\n", arg);
        return 1;
    }

    SmallclueLineVector vec = {0};
    int status = 0;
    if (index >= argc) {
        status = smallclueLineVectorLoadStream(stdin, NULL, "sort", &vec);
    } else {
        for (int i = index; i < argc && status == 0; ++i) {
            FILE *fp = fopen(argv[i], "r");
            if (!fp) {
                fprintf(stderr, "sort: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                break;
            }
            status = smallclueLineVectorLoadStream(fp, argv[i], "sort", &vec);
            fclose(fp);
        }
    }
    if (status == 0 && vec.count > 1) {
        qsort(vec.items, vec.count, sizeof(char *), smallclueStringCompare);
    }
    if (status == 0) {
        if (reverse) {
            for (size_t i = vec.count; i-- > 0;) {
                fputs(vec.items[i], stdout);
            }
        } else {
            for (size_t i = 0; i < vec.count; ++i) {
                fputs(vec.items[i], stdout);
            }
        }
    }
    smallclueLineVectorFree(&vec);
    return status;
}

static int smallclueUniqStream(FILE *fp, const char *path, int print_counts) {
    char *line = NULL;
    size_t cap = 0;
    char *prev = NULL;
    long count = 0;
    int status = 0;
    while (true) {
        ssize_t len = getline(&line, &cap, fp);
        if (len < 0) {
            if (!feof(fp)) {
                fprintf(stderr, "uniq: %s: %s\n", path ? path : "(stdin)", strerror(errno));
                status = 1;
            }
            break;
        }
        if (!prev || strcmp(prev, line) != 0) {
            if (prev) {
                if (print_counts) {
                    printf("%7ld %s", count, prev);
                } else {
                    fputs(prev, stdout);
                }
                free(prev);
            }
            prev = strdup(line);
            if (!prev) {
                fprintf(stderr, "uniq: out of memory\n");
                status = 1;
                break;
            }
            count = 1;
        } else {
            count++;
        }
    }
    if (status == 0 && prev) {
        if (print_counts) {
            printf("%7ld %s", count, prev);
        } else {
            fputs(prev, stdout);
        }
    }
    free(prev);
    free(line);
    return status;
}

static int smallclueUniqCommand(int argc, char **argv) {
    int print_counts = 0;
    int index = 1;
    while (index < argc) {
        const char *arg = argv[index];
        if (!arg || arg[0] != '-') {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }
        if (strcmp(arg, "-c") == 0) {
            print_counts = 1;
            index++;
            continue;
        }
        fprintf(stderr, "uniq: unsupported option '%s'\n", arg);
        return 1;
    }
    if (index >= argc) {
        return smallclueUniqStream(stdin, "(stdin)", print_counts);
    }
    int status = 0;
    for (int i = index; i < argc; ++i) {
        FILE *fp = fopen(argv[i], "r");
        if (!fp) {
            fprintf(stderr, "uniq: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        status |= smallclueUniqStream(fp, argv[i], print_counts);
        fclose(fp);
    }
    return status;
}

static bool smallclueSedParseExpr(const char *expr, char **pattern, char **replacement, bool *global) {
    if (!expr || expr[0] != 's' || expr[1] == '\0') {
        return false;
    }
    char delim = expr[1];
    const char *pat_start = expr + 2;
    const char *pat_end = strchr(pat_start, delim);
    if (!pat_end) {
        return false;
    }
    const char *rep_start = pat_end + 1;
    const char *rep_end = strchr(rep_start, delim);
    if (!rep_end) {
        return false;
    }
    size_t pat_len = (size_t)(pat_end - pat_start);
    size_t rep_len = (size_t)(rep_end - rep_start);
    *pattern = (char *)malloc(pat_len + 1);
    *replacement = (char *)malloc(rep_len + 1);
    if (!*pattern || !*replacement) {
        free(*pattern);
        free(*replacement);
        return false;
    }
    memcpy(*pattern, pat_start, pat_len);
    (*pattern)[pat_len] = '\0';
    memcpy(*replacement, rep_start, rep_len);
    (*replacement)[rep_len] = '\0';
    *global = (strchr(rep_end + 1, 'g') != NULL);
    return true;
}

static char *smallclueSedApply(const char *line, const char *pattern, const char *replacement, bool global) {
    size_t pat_len = strlen(pattern);
    size_t rep_len = strlen(replacement);
    size_t line_len = strlen(line);
    size_t cap = line_len + 1 + ((rep_len > pat_len) ? (rep_len - pat_len) * 4 : 0);
    char *out = (char *)malloc(cap);
    if (!out) {
        return NULL;
    }
    size_t out_len = 0;
    const char *cursor = line;
    bool replaced = false;
    while (*cursor) {
        if (pat_len > 0 && strncmp(cursor, pattern, pat_len) == 0) {
            size_t needed = out_len + rep_len + (line_len - (cursor - line)) + 1;
            if (needed > cap) {
                cap = needed + 32;
                char *resized = (char *)realloc(out, cap);
                if (!resized) {
                    free(out);
                    return NULL;
                }
                out = resized;
            }
            memcpy(out + out_len, replacement, rep_len);
            out_len += rep_len;
            cursor += pat_len;
            replaced = true;
            if (!global) {
                memcpy(out + out_len, cursor, strlen(cursor) + 1);
                return out;
            }
            continue;
        }
        if (out_len + 2 > cap) {
            cap *= 2;
            char *resized = (char *)realloc(out, cap);
            if (!resized) {
                free(out);
                return NULL;
            }
            out = resized;
        }
        out[out_len++] = *cursor++;
    }
    out[out_len] = '\0';
    if (!replaced) {
        free(out);
        return strdup(line);
    }
    return out;
}

static int smallclueSedCommand(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "sed: missing expression\n");
        return 1;
    }
    char *pattern = NULL;
    char *replacement = NULL;
    bool global = false;
    if (!smallclueSedParseExpr(argv[1], &pattern, &replacement, &global)) {
        fprintf(stderr, "sed: invalid expression '%s'\n", argv[1]);
        return 1;
    }
    int status = 0;
    char *line = NULL;
    size_t cap = 0;
    int index = 2;
    if (index >= argc) {
        while (!status) {
            ssize_t len = getline(&line, &cap, stdin);
            if (len < 0) {
                if (!feof(stdin)) {
                    perror("sed");
                    status = 1;
                }
                break;
            }
            char *out = smallclueSedApply(line, pattern, replacement, global);
            if (!out) {
                fprintf(stderr, "sed: out of memory\n");
                status = 1;
                break;
            }
            fputs(out, stdout);
            free(out);
        }
    } else {
        for (int i = index; i < argc && status == 0; ++i) {
            FILE *fp = fopen(argv[i], "r");
            if (!fp) {
                fprintf(stderr, "sed: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                break;
            }
            while (true) {
                ssize_t len = getline(&line, &cap, fp);
                if (len < 0) {
                    if (!feof(fp)) {
                        fprintf(stderr, "sed: %s: %s\n", argv[i], strerror(errno));
                        status = 1;
                    }
                    break;
                }
                char *out = smallclueSedApply(line, pattern, replacement, global);
                if (!out) {
                    fprintf(stderr, "sed: out of memory\n");
                    status = 1;
                    break;
                }
                fputs(out, stdout);
                free(out);
            }
            fclose(fp);
        }
    }
    free(pattern);
    free(replacement);
    free(line);
    return status;
}

static void smallclueCutPrintField(const char *line, char delim, int field) {
    if (field <= 0) {
        return;
    }
    int current = 1;
    const char *start = line;
    const char *ptr = line;
    while (true) {
        if (*ptr == delim || *ptr == '\0' || *ptr == '\n') {
            if (current == field) {
                size_t slice = (size_t)(ptr - start);
                fwrite(start, 1, slice, stdout);
                if (slice == 0 || start[slice - 1] != '\n') {
                    putchar('\n');
                }
                return;
            }
            if (*ptr == '\0') {
                break;
            }
            current++;
            start = ptr + 1;
        }
        if (*ptr == '\0') {
            break;
        }
        ptr++;
    }
    putchar('\n');
}

static int smallclueCutCommand(int argc, char **argv) {
    char delimiter = '\t';
    int field = -1;
    int index = 1;
    while (index < argc) {
        const char *arg = argv[index];
        if (!arg || arg[0] != '-') {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }
        if (strcmp(arg, "-d") == 0) {
            if (index + 1 >= argc || !argv[index + 1][0]) {
                fprintf(stderr, "cut: missing delimiter\n");
                return 1;
            }
            delimiter = argv[index + 1][0];
            index += 2;
            continue;
        }
        if (strcmp(arg, "-f") == 0) {
            if (index + 1 >= argc) {
                fprintf(stderr, "cut: missing field number\n");
                return 1;
            }
            field = (int)smallclueParseLong(argv[index + 1]);
            if (field <= 0) {
                fprintf(stderr, "cut: invalid field '%s'\n", argv[index + 1]);
                return 1;
            }
            index += 2;
            continue;
        }
        fprintf(stderr, "cut: unsupported option '%s'\n", arg);
        return 1;
    }
    if (field <= 0) {
        fprintf(stderr, "cut: missing -f option\n");
        return 1;
    }
    char *line = NULL;
    size_t cap = 0;
    int status = 0;
    if (index >= argc) {
        while (true) {
            ssize_t len = getline(&line, &cap, stdin);
            if (len < 0) {
                if (!feof(stdin)) {
                    perror("cut");
                    status = 1;
                }
                break;
            }
            smallclueCutPrintField(line, delimiter, field);
        }
    } else {
        for (int i = index; i < argc; ++i) {
            FILE *fp = fopen(argv[i], "r");
            if (!fp) {
                fprintf(stderr, "cut: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                continue;
            }
            while (true) {
                ssize_t len = getline(&line, &cap, fp);
                if (len < 0) {
                    if (!feof(fp)) {
                        fprintf(stderr, "cut: %s: %s\n", argv[i], strerror(errno));
                        status = 1;
                    }
                    break;
                }
                smallclueCutPrintField(line, delimiter, field);
            }
            fclose(fp);
        }
    }
    free(line);
    return status;
}

static int smallclueTrCommand(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "tr: missing operand\n");
        return 1;
    }
    const char *set1 = argv[1];
    const char *set2 = argv[2];
    size_t len1 = strlen(set1);
    size_t len2 = strlen(set2);
    unsigned char map[256];
    bool delete_map[256];
    bool delete_only = (len2 == 0);
    for (int i = 0; i < 256; ++i) {
        map[i] = (unsigned char)i;
        delete_map[i] = false;
    }
    if (delete_only) {
        for (size_t i = 0; i < len1; ++i) {
            delete_map[(unsigned char)set1[i]] = true;
        }
    } else {
        for (size_t i = 0; i < len1; ++i) {
            unsigned char from = (unsigned char)set1[i];
            unsigned char to = (unsigned char)(i < len2 ? set2[i] : set2[len2 - 1]);
            map[from] = to;
        }
    }
    int ch;
    while ((ch = getchar()) != EOF) {
        unsigned char c = (unsigned char)ch;
        if (delete_only) {
            if (delete_map[c]) {
                continue;
            }
            putchar(c);
        } else {
            putchar(map[c]);
        }
    }
    return 0;
}

static int smallclueIdCommand(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        fprintf(stderr, "id: no user lookup support in smallclue\n");
    }
    uid_t uid = getuid();
    uid_t euid = geteuid();
    gid_t gid = getgid();
    gid_t egid = getegid();
    struct passwd *pw = getpwuid(uid);
    struct passwd *epw = getpwuid(euid);
    struct group *gr = getgrgid(gid);
    struct group *egr = getgrgid(egid);
    printf("uid=%u(%s) gid=%u(%s)", (unsigned)uid, pw ? pw->pw_name : "?", (unsigned)gid, gr ? gr->gr_name : "?");
    if (euid != uid) {
        printf(" euid=%u(%s)", (unsigned)euid, epw ? epw->pw_name : "?");
    }
    if (egid != gid) {
        printf(" egid=%u(%s)", (unsigned)egid, egr ? egr->gr_name : "?");
    }
    int ngroups = getgroups(0, NULL);
    if (ngroups > 0) {
        gid_t *groups = (gid_t *)malloc((size_t)ngroups * sizeof(gid_t));
        if (groups && getgroups(ngroups, groups) >= 0) {
            printf(" groups=");
            for (int i = 0; i < ngroups; ++i) {
                struct group *gg = getgrgid(groups[i]);
                if (i > 0) {
                    putchar(',');
                }
                printf("%u(%s)", (unsigned)groups[i], gg ? gg->gr_name : "?");
            }
        }
        free(groups);
    }
    putchar('\n');
    return 0;
}

static int smallclueEnvCommand(int argc, char **argv) {
    smallclueResetGetopt();
    int clear_env = 0;
    int opt;
    while ((opt = getopt(argc, argv, "i")) != -1) {
        if (opt == 'i') {
            clear_env = 1;
        } else {
            fprintf(stderr, "usage: env [-i] [name=value ...] [command [args...]]\n");
            return 1;
        }
    }
    if (clear_env) {
        smallclueEnvClearAll();
    }
    int index = optind;
    while (index < argc) {
        const char *arg = argv[index];
        const char *eq = arg ? strchr(arg, '=') : NULL;
        if (!eq) {
            break;
        }
        if (eq == arg) {
            fprintf(stderr, "env: invalid assignment '%s'\n", arg);
            return 1;
        }
        size_t name_len = (size_t)(eq - arg);
        char *name = (char *)malloc(name_len + 1);
        if (!name) {
            perror("env");
            return 1;
        }
        memcpy(name, arg, name_len);
        name[name_len] = '\0';
        const char *value = eq + 1;
        if (setenv(name, value, 1) != 0) {
            fprintf(stderr, "env: failed to set %s: %s\n", name, strerror(errno));
            free(name);
            return 1;
        }
        free(name);
        index++;
    }
    if (index >= argc) {
        extern char **environ;
        if (environ) {
            for (char **envp = environ; *envp; ++envp) {
                puts(*envp);
            }
        }
        return 0;
    }
    execvp(argv[index], &argv[index]);
    fprintf(stderr, "env: %s: %s\n", argv[index], strerror(errno));
    if (errno == ENOENT) {
        return 127;
    }
    return 126;
}

static int smallclueGrepCommand(int argc, char **argv) {
    int index = 1;
    int number_lines = 0;
    int ignore_case = 0;
    int invert_match = 0;
    while (index < argc) {
        const char *arg = argv[index];
        if (!arg || arg[0] != '-') {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }
        if (strncmp(arg, "--", 2) == 0) {
            /* Support common long forms and treat unknown long opts as end of options. */
            if (strcmp(arg, "--ignore-case") == 0 || strcmp(arg, "--ignore") == 0) {
                ignore_case = 1;
                index++;
                continue;
            }
            if (strcmp(arg, "--invert-match") == 0 || strcmp(arg, "--invert") == 0) {
                invert_match = 1;
                index++;
                continue;
            }
            if (strcmp(arg, "--line-number") == 0 || strcmp(arg, "--number") == 0) {
                number_lines = 1;
                index++;
                continue;
            }
            if (strncmp(arg, "--color", 7) == 0 || strncmp(arg, "--colour", 8) == 0) {
                /* Accept --color[=auto|never|always] without changing behaviour. */
                index++;
                continue;
            }
            /* Unrecognized long option: treat as start of pattern/paths. */
            break;
        }
        for (const char *opt = arg + 1; *opt; ++opt) {
            if (*opt == 'n') {
                number_lines = 1;
            } else if (*opt == 'i') {
                ignore_case = 1;
            } else if (*opt == 'v') {
                invert_match = 1;
            } else {
                fprintf(stderr, "grep: unsupported option -%c\n", *opt);
                return 1;
            }
        }
        index++;
    }
    if (index >= argc) {
        fprintf(stderr, "grep: missing pattern\n");
        return 1;
    }
    const char *pattern = argv[index++];
    int paths = argc - index;
    int status = 1;
    char *line = NULL;
    size_t cap = 0;
    if (paths <= 0) {
        ssize_t len;
        long line_no = 0;
        while ((len = getline(&line, &cap, stdin)) != -1) {
            line_no++;
            int found = smallclueStrCaseStr(line, pattern, ignore_case) != NULL;
            if (invert_match ? !found : found) {
                if (number_lines) {
                    printf("%ld:", line_no);
                }
                fwrite(line, 1, (size_t)len, stdout);
                status = 0;
            }
        }
    } else {
        for (int i = index; i < argc; ++i) {
            const char *path = argv[i];
            FILE *fp = fopen(path, "r");
            if (!fp) {
                fprintf(stderr, "grep: %s: %s\n", path, strerror(errno));
                continue;
            }
            ssize_t len;
            long line_no = 0;
            while ((len = getline(&line, &cap, fp)) != -1) {
                line_no++;
                int found = smallclueStrCaseStr(line, pattern, ignore_case) != NULL;
                if (invert_match ? !found : found) {
                    if (paths > 1) {
                        printf("%s:", path);
                    }
                    if (number_lines) {
                        printf("%ld:", line_no);
                    }
                    fwrite(line, 1, (size_t)len, stdout);
                    status = 0;
                }
            }
            fclose(fp);
        }
    }
    free(line);
    return status;
}

typedef struct {
    uint64_t lines;
    uint64_t words;
    uint64_t bytes;
} SmallclueWcCounts;

static int smallclueWcProcessFile(const char *path, SmallclueWcCounts *counts) {
    FILE *fp = NULL;
    if (path) {
        fp = fopen(path, "r");
        if (!fp) {
            fprintf(stderr, "wc: %s: %s\n", path, strerror(errno));
            return 1;
        }
    } else {
        fp = stdin;
    }
    int c;
    int in_word = 0;
    counts->lines = counts->words = counts->bytes = 0;
    while ((c = fgetc(fp)) != EOF) {
        counts->bytes++;
        if (c == '\n') {
            counts->lines++;
        }
        if (isspace(c)) {
            in_word = 0;
        } else if (!in_word) {
            counts->words++;
            in_word = 1;
        }
    }
    if (fp != stdin) {
        fclose(fp);
    }
    if (ferror(fp)) {
        fprintf(stderr, "wc: %s: read error\n", path ? path : "(stdin)");
        return 1;
    }
    return 0;
}

static void smallclueWcPrint(const SmallclueWcCounts *counts, int show_lines, int show_words, int show_bytes, const char *label) {
    if (show_lines) {
        printf("%12" PRIu64, counts->lines);
    }
    if (show_words) {
        printf("%12" PRIu64, counts->words);
    }
    if (show_bytes) {
        printf("%12" PRIu64, counts->bytes);
    }
    if (label) {
        printf(" %s", label);
    }
    putchar('\n');
}

static int smallclueWcCommand(int argc, char **argv) {
    int show_lines = 0, show_words = 0, show_bytes = 0;
    int index = 1;
    while (index < argc) {
        const char *arg = argv[index];
        if (!arg || arg[0] != '-') {
            break;
        }
        if (strcmp(arg, "--") == 0) {
            index++;
            break;
        }
        for (const char *opt = arg + 1; *opt; ++opt) {
            if (*opt == 'l') show_lines = 1;
            else if (*opt == 'w') show_words = 1;
            else if (*opt == 'c') show_bytes = 1;
            else {
                fprintf(stderr, "wc: invalid option -- %c\n", *opt);
                return 1;
            }
        }
        index++;
    }
    if (!show_lines && !show_words && !show_bytes) {
        show_lines = show_words = show_bytes = 1;
    }
    int paths = argc - index;
    int status = 0;
    SmallclueWcCounts counts;
    SmallclueWcCounts total = {0, 0, 0};
    if (paths <= 0) {
        if (smallclueWcProcessFile(NULL, &counts) != 0) {
            return 1;
        }
        smallclueWcPrint(&counts, show_lines, show_words, show_bytes, NULL);
    } else {
        for (int i = index; i < argc; ++i) {
            if (smallclueWcProcessFile(argv[i], &counts) != 0) {
                status = 1;
                continue;
            }
            smallclueWcPrint(&counts, show_lines, show_words, show_bytes, argv[i]);
            total.lines += counts.lines;
            total.words += counts.words;
            total.bytes += counts.bytes;
        }
        if (paths > 1) {
            smallclueWcPrint(&total, show_lines, show_words, show_bytes, "total");
        }
    }
    return status;
}

typedef struct {
    int summarize_only;
    int use_kilobytes;
    int human_readable;
} SmallclueDuOptions;

static void smallclueDuPrintSize(long long bytes,
                                const char *path,
                                const SmallclueDuOptions *opts) {
    if (opts && opts->human_readable) {
        static const char units[] = {'B', 'K', 'M', 'G', 'T', 'P', 'E'};
        double value = (double)bytes;
        size_t unit = 0;
        while (value >= 1024.0 && unit < (sizeof(units) / sizeof(units[0])) - 1) {
            value /= 1024.0;
            unit++;
        }
        if (unit == 0 || value >= 10.0) {
            printf("%.0f%c\t%s\n", value, units[unit], path);
        } else {
            printf("%.1f%c\t%s\n", value, units[unit], path);
        }
        return;
    }

    long long value = bytes;
    if (opts && opts->use_kilobytes) {
        if (value >= 0) {
            value = (value + 1023) / 1024;
        } else {
            value = -(((-value) + 1023) / 1024);
        }
    }
    printf("%lld\t%s\n", value, path);
}

static long long smallclueDuVisit(const char *path,
                                 int *status,
                                 const SmallclueDuOptions *opts,
                                 int depth) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "du: %s: %s\n", path, strerror(errno));
        if (status) *status = 1;
        return 0;
    }
    long long total = st.st_size;
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "du: %s: %s\n", path, strerror(errno));
            if (status) *status = 1;
            return total;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child[PATH_MAX];
            if (smallclueBuildPath(child, sizeof(child), path, entry->d_name) != 0) {
                fprintf(stderr, "du: %s/%s: %s\n", path, entry->d_name, strerror(errno));
                if (status) *status = 1;
                continue;
            }
            total += smallclueDuVisit(child, status, opts, depth + 1);
        }
        closedir(dir);
    }
    if (!opts || !opts->summarize_only || depth == 0) {
        smallclueDuPrintSize(total, path, opts);
    }
    return total;
}

static int smallclueDuCommand(int argc, char **argv) {
    SmallclueDuOptions opts = {0, 0, 0};
    int opt;
    smallclueResetGetopt();
    while ((opt = getopt(argc, argv, "skh")) != -1) {
        switch (opt) {
            case 's':
                opts.summarize_only = 1;
                break;
            case 'k':
                opts.use_kilobytes = 1;
                break;
            case 'h':
                opts.human_readable = 1;
                break;
            default:
                return 1;
        }
    }

    int status = 0;
    if (optind >= argc) {
        smallclueDuVisit(".", &status, &opts, 0);
    } else {
        for (int i = optind; i < argc; ++i) {
            smallclueDuVisit(argv[i], &status, &opts, 0);
        }
    }
    return status ? 1 : 0;
}

static int smallclueFindVisit(const char *path, const char *pattern, int *status) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        if (status) *status = 1;
        return 1;
    }
    const char *leaf = smallclueLeafName(path);
    if (!pattern || fnmatch(pattern, leaf, 0) == 0) {
        printf("%s\n", path);
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
            if (status) *status = 1;
            return 1;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child[PATH_MAX];
            if (smallclueBuildPath(child, sizeof(child), path, entry->d_name) != 0) {
                fprintf(stderr, "find: %s/%s: %s\n", path, entry->d_name, strerror(errno));
                if (status) *status = 1;
                continue;
            }
            smallclueFindVisit(child, pattern, status);
        }
        closedir(dir);
    }
    return 0;
}

static int smallclueFindCommand(int argc, char **argv) {
    const char *start = ".";
    const char *pattern = NULL;
    int index = 1;
    if (index < argc && argv[index] && argv[index][0] != '-') {
        start = argv[index++];
    }
    while (index < argc) {
        const char *arg = argv[index++];
        if (strcmp(arg, "-name") == 0) {
            if (index >= argc) {
                fprintf(stderr, "find: missing argument to -name\n");
                return 1;
            }
            pattern = argv[index++];
        } else {
            fprintf(stderr, "find: unsupported predicate '%s'\n", arg);
            return 1;
        }
    }
    int status = 0;
    smallclueFindVisit(start, pattern, &status);
    return status ? 1 : 0;
}

static const char *smallclueLeafName(const char *path) {
    if (!path) {
        return "";
    }
    const char *start = path;
    const char *end = path + strlen(path);
    while (end > start && end[-1] == '/') {
        --end;
    }
    if (end == start) {
        return path;
    }
    const char *leaf = end;
    while (leaf > start && leaf[-1] != '/') {
        --leaf;
    }
    return leaf;
}

static int smallclueBuildPath(char *buf, size_t buf_size, const char *dir, const char *leaf) {
    if (!buf || buf_size == 0 || !dir || !leaf) {
        errno = EINVAL;
        return -1;
    }
    size_t dir_len = strlen(dir);
    int need_slash = (dir_len > 0 && dir[dir_len - 1] != '/');
    int written = snprintf(buf, buf_size, need_slash ? "%s/%s" : "%s%s", dir, leaf);
    if (written < 0 || (size_t)written >= buf_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static void smallclueTrimTrailingSlashes(char *path) {
    if (!path) {
        return;
    }
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
    if (len == 1 && path[0] == '/') {
        path[1] = '\0';
    }
}

static bool smallclueChopParentDirectory(char *path) {
    if (!path || path[0] == '\0') {
        return false;
    }
    smallclueTrimTrailingSlashes(path);
    size_t len = strlen(path);
    if (len == 0) {
        return false;
    }
    char *slash = strrchr(path, '/');
    if (!slash) {
        path[0] = '\0';
        return false;
    }
    if (slash == path) {
        return false;
    }
    *slash = '\0';
    smallclueTrimTrailingSlashes(path);
    return path[0] != '\0';
}

static bool smallclueConfirmDelete(const char *label, const char *path) {
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "%s: cannot prompt on non-interactive input for '%s' (use -f to force)\n",
                label, path);
        return false;
    }
    fprintf(stderr, "%s: remove '%s'? [y/N] ", label, path);
    fflush(stderr);
    int c = getchar();
    /* consume the rest of the line */
    int d;
    while ((d = getchar()) != '\n' && d != EOF) { }
    return c == 'y' || c == 'Y';
}

static int smallclueRemovePathWithLabel(const char *label, const char *path, bool recursive, bool force) {
    const char *target = path;
#if defined(PSCAL_TARGET_IOS)
    char expanded[PATH_MAX];
    if (path && pathTruncateExpand(path, expanded, sizeof(expanded))) {
        target = expanded;
    }
#endif
    struct stat st;
    if (lstat(target, &st) != 0) {
        if (!force) {
            fprintf(stderr, "%s: %s: %s\n", label, target, strerror(errno));
        }
        return force ? 0 : -1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "%s: %s: is a directory\n", label, target);
            return -1;
        }
        if (!force && !smallclueConfirmDelete(label, target)) {
            return 1;
        }
        DIR *dir = opendir(target);
        if (!dir) {
            fprintf(stderr, "%s: %s: %s\n", label, target, strerror(errno));
            return -1;
        }
        struct dirent *entry;
        int status = 0;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child_path[PATH_MAX];
            if (smallclueBuildPath(child_path, sizeof(child_path), path, entry->d_name) != 0) {
                fprintf(stderr, "%s: %s/%s: %s\n", label, target, entry->d_name, strerror(errno));
                status = -1;
                break;
            }
            if (smallclueRemovePathWithLabel(label, child_path, true, force) != 0) {
                status = -1;
            }
        }
        closedir(dir);
        if (status != 0) {
            return -1;
        }
        if (rmdir(target) != 0) {
            fprintf(stderr, "%s: %s: %s\n", label, target, strerror(errno));
            return -1;
        }
        return 0;
    }
    if (unlink(target) != 0) {
        if (!force) {
            fprintf(stderr, "%s: %s: %s\n", label, target, strerror(errno));
            return -1;
        }
        return 0;
    }
    return 0;
}

static int smallclueCopyFile(const char *label, const char *src, const char *dst) {
    char resolved_src[PATH_MAX];
    char resolved_dst[PATH_MAX];
    const char *src_path = smallclueResolvePath(src, resolved_src, sizeof(resolved_src));
    const char *dst_path = smallclueResolvePath(dst, resolved_dst, sizeof(resolved_dst));

    int in_fd = open(src_path, O_RDONLY);
    if (in_fd < 0) {
        fprintf(stderr, "%s: %s: %s\n", label, src, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(in_fd, &st) != 0) {
        fprintf(stderr, "%s: %s: %s\n", label, src, strerror(errno));
        close(in_fd);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "%s: %s: unsupported file type\n", label, src);
        close(in_fd);
        return -1;
    }
    mode_t mode = st.st_mode & 0777;
    int out_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out_fd < 0) {
        fprintf(stderr, "%s: %s: %s\n", label, dst, strerror(errno));
        close(in_fd);
        return -1;
    }
    char buffer[16384];
    ssize_t nread;
    int status = 0;
    while ((nread = read(in_fd, buffer, sizeof(buffer))) > 0) {
        ssize_t written = 0;
        while (written < nread) {
            ssize_t nwrite = write(out_fd, buffer + written, (size_t)(nread - written));
            if (nwrite < 0) {
                fprintf(stderr, "%s: %s: %s\n", label, dst, strerror(errno));
                status = -1;
                break;
            }
            written += nwrite;
        }
        if (status != 0) {
            break;
        }
    }
    if (nread < 0) {
        fprintf(stderr, "%s: %s: %s\n", label, src, strerror(errno));
        status = -1;
    }
    if (close(out_fd) != 0) {
        fprintf(stderr, "%s: %s: %s\n", label, dst, strerror(errno));
        status = -1;
    }
    close(in_fd);
    if (status != 0) {
        unlink(dst);
    }
    return status;
}

static int smallclueMkdirParents(const char *path, mode_t mode) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    char *mutable_path = strdup(path);
    if (!mutable_path) {
        errno = ENOMEM;
        return -1;
    }
    size_t len = strlen(mutable_path);
    while (len > 1 && mutable_path[len - 1] == '/') {
        mutable_path[len - 1] = '\0';
        len--;
    }
    if (len == 0) {
        free(mutable_path);
        errno = EINVAL;
        return -1;
    }
    for (char *cursor = mutable_path + 1; *cursor; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mutable_path[0] != '\0') {
                if (mkdir(mutable_path, mode) != 0 && errno != EEXIST) {
                    int err = errno;
                    free(mutable_path);
                    errno = err;
                    return -1;
                }
            }
            *cursor = '/';
            while (*(cursor + 1) == '/') {
                cursor++;
            }
        }
    }
    if (mkdir(path, mode) != 0) {
        if (errno == EEXIST) {
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                free(mutable_path);
                return 0;
            }
        }
        int err = errno;
        free(mutable_path);
        errno = err;
        return -1;
    }
    free(mutable_path);
    return 0;
}

static int smallclueRmCommand(int argc, char **argv) {
    int recursive = 0;
    int force = 0;
    int opt;
    smallclueResetGetopt();
    while ((opt = getopt(argc, argv, "rf")) != -1) {
        switch (opt) {
            case 'r':
                recursive = 1;
                break;
            case 'f':
                force = 1;
                break;
            default:
                fprintf(stderr, "rm: invalid option -- %c\n", optopt);
                return 1;
        }
    }
    if (optind >= argc) {
        if (!force) {
            fprintf(stderr, "rm: missing operand\n");
            return 1;
        }
        return 0;
    }
    int status = 0;
    for (int i = optind; i < argc; ++i) {
        const char *input = argv[i];
        const char *expanded = input;
#if defined(PSCAL_TARGET_IOS)
        char pathbuf[PATH_MAX];
        if (pathTruncateExpand(input, pathbuf, sizeof(pathbuf))) {
            expanded = pathbuf;
        }
#endif
        if (strpbrk(expanded, "*?[")) {
            glob_t matches;
            memset(&matches, 0, sizeof(matches));
            int gret = glob(expanded, GLOB_NOCHECK, NULL, &matches);
            if (gret != 0) {
                globfree(&matches);
                if (!force) {
                    status = 1;
                }
                continue;
            }
            for (size_t m = 0; m < matches.gl_pathc; ++m) {
                if (smallclueRemovePathWithLabel("rm", matches.gl_pathv[m], recursive != 0, force != 0) != 0) {
                    if (!force) {
                        status = 1;
                    }
                }
            }
            globfree(&matches);
        } else {
            if (smallclueRemovePathWithLabel("rm", expanded, recursive != 0, force != 0) != 0) {
                if (!force) {
                    status = 1;
                }
            }
        }
    }
    return status;
}

static int smallclueRmdirPath(const char *path, bool parents) {
    if (rmdir(path) != 0) {
        fprintf(stderr, "rmdir: %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (!parents) {
        return 0;
    }
    char *mutable_path = strdup(path);
    if (!mutable_path) {
        fprintf(stderr, "rmdir: %s\n", strerror(errno));
        return -1;
    }
    while (smallclueChopParentDirectory(mutable_path)) {
        if (mutable_path[0] == '\0' || strcmp(mutable_path, ".") == 0 ||
            strcmp(mutable_path, "/") == 0) {
            break;
        }
        if (rmdir(mutable_path) != 0) {
            fprintf(stderr, "rmdir: %s: %s\n", mutable_path, strerror(errno));
            free(mutable_path);
            return -1;
        }
    }
    free(mutable_path);
    return 0;
}

static int smallclueRmdirCommand(int argc, char **argv) {
    int parents = 0;
    int opt;
    smallclueResetGetopt();
    while ((opt = getopt(argc, argv, "p")) != -1) {
        if (opt == 'p') {
            parents = 1;
        } else {
            fprintf(stderr, "usage: rmdir [-p] dir...\n");
            return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "rmdir: missing operand\n");
        return 1;
    }
    int status = 0;
    for (int i = optind; i < argc; ++i) {
        if (smallclueRmdirPath(argv[i], parents != 0) != 0) {
            status = 1;
        }
    }
    return status;
}

static int smallclueMkdirCommand(int argc, char **argv) {
    int parents = 0;
    int opt;
    smallclueResetGetopt();
    while ((opt = getopt(argc, argv, "p")) != -1) {
        switch (opt) {
            case 'p':
                parents = 1;
                break;
            default:
                fprintf(stderr, "mkdir: invalid option -- %c\n", optopt);
                return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "mkdir: missing operand\n");
        return 1;
    }
    int status = 0;
    for (int i = optind; i < argc; ++i) {
        const char *target = argv[i];
        if (parents) {
            if (smallclueMkdirParents(target, 0777) != 0) {
                fprintf(stderr, "mkdir: %s: %s\n", target, strerror(errno));
                status = 1;
            }
        } else {
            if (mkdir(target, 0777) != 0) {
                fprintf(stderr, "mkdir: %s: %s\n", target, strerror(errno));
                status = 1;
            }
        }
    }
    return status;
}

static int smallclueFileCommand(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "file: missing operand\n");
        return 1;
    }
    int status = 0;
    unsigned char buffer[512];
    for (int i = 1; i < argc; ++i) {
        const char *path = argv[i];
        struct stat st;
        if (lstat(path, &st) != 0) {
            fprintf(stderr, "file: %s: %s\n", path, strerror(errno));
            status = 1;
            continue;
        }
        printf("%s: ", path);
        if (S_ISDIR(st.st_mode)) {
            printf("directory\n");
        } else if (S_ISLNK(st.st_mode)) {
            char target[PATH_MAX];
            ssize_t len = readlink(path, target, sizeof(target) - 1);
            if (len >= 0) {
                target[len] = '\0';
                printf("symbolic link to '%s'\n", target);
            } else {
                printf("symbolic link (unreadable target)\n");
            }
        } else if (S_ISCHR(st.st_mode)) {
            printf("character device\n");
        } else if (S_ISBLK(st.st_mode)) {
            printf("block device\n");
        } else if (S_ISFIFO(st.st_mode)) {
            printf("named pipe\n");
        } else if (S_ISSOCK(st.st_mode)) {
            printf("socket\n");
        } else if (S_ISREG(st.st_mode)) {
            FILE *fp = fopen(path, "rb");
            if (!fp) {
                printf("regular file (unreadable)\n");
                status = 1;
                continue;
            }
            size_t read_bytes = fread(buffer, 1, sizeof(buffer), fp);
            fclose(fp);
            int is_text = 1;
            for (size_t b = 0; b < read_bytes; ++b) {
                unsigned char c = buffer[b];
                if (c == 0 || (c < 0x09) || (c > 0x0D && c < 0x20 && c != 0x1B)) {
                    is_text = 0;
                    break;
                }
            }
            printf(is_text ? "ASCII text\n" : "binary data\n");
        } else {
            printf("unknown file type\n");
        }
    }
    return status;
}

static int smallclueLnCommand(int argc, char **argv) {
    int symbolic = 0;
    int opt;
    smallclueResetGetopt();
    while ((opt = getopt(argc, argv, "s")) != -1) {
        switch (opt) {
            case 's':
                symbolic = 1;
                break;
            default:
                fprintf(stderr, "ln: invalid option -- %c\n", optopt);
                return 1;
        }
    }
    if (argc - optind < 2) {
        fprintf(stderr, "ln: missing file operand\n");
        return 1;
    }
    const char *target = argv[optind];
    const char *linkname = argv[optind + 1];
    int status = 0;
    if (symbolic) {
        if (symlink(target, linkname) != 0) {
            fprintf(stderr, "ln: cannot create symbolic link '%s': %s\n", linkname, strerror(errno));
            status = 1;
        }
    } else {
        if (link(target, linkname) != 0) {
            fprintf(stderr, "ln: cannot create link '%s': %s\n", linkname, strerror(errno));
            status = 1;
        }
    }
    return status;
}

static char *smallclueSearchPath(const char *name) {
    if (!name || !*name) {
        return NULL;
    }
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) {
            return strdup(name);
        }
        return NULL;
    }
    const char *env = getenv("PATH");
    if (!env || !*env) {
        return NULL;
    }
    char *copy = strdup(env);
    if (!copy) {
        return NULL;
    }
    char *token = strtok(copy, ":");
    while (token) {
        char candidate[PATH_MAX];
        if (snprintf(candidate, sizeof(candidate), "%s/%s", token, name) < (int)sizeof(candidate)) {
            if (access(candidate, X_OK) == 0) {
                char *result = strdup(candidate);
                free(copy);
                return result;
            }
        }
        token = strtok(NULL, ":");
    }
    free(copy);
    return NULL;
}

static int smallclueTypeCommand(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "type: missing operand\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        const char *name = argv[i];
        const SmallclueApplet *applet = smallclueFindApplet(name);
        if (applet) {
            printf("%s is a smallclue applet\n", name);
            continue;
        }
        char *path = smallclueSearchPath(name);
        if (path) {
            printf("%s is %s\n", name, path);
            free(path);
        } else {
            fprintf(stderr, "type: %s not found\n", name);
            status = 1;
        }
    }
    return status;
}

static int smallclueCpCommand(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "cp: missing file operand\n");
        return 1;
    }
    const char *dest = argv[argc - 1];
    char resolved_dest_root[PATH_MAX];
    const char *dest_real = smallclueResolvePath(dest, resolved_dest_root, sizeof(resolved_dest_root));
    struct stat dest_stat;
    int dest_exists = (stat(dest_real, &dest_stat) == 0);
    bool dest_is_dir = dest_exists && S_ISDIR(dest_stat.st_mode);
    int source_count = argc - 2;
    if (source_count > 1 && !dest_is_dir) {
        fprintf(stderr, "cp: target '%s' is not a directory\n", dest);
        return 1;
    }
    int status = 0;
    for (int i = 1; i <= source_count; ++i) {
        char resolved_src[PATH_MAX];
        const char *src = smallclueResolvePath(argv[i], resolved_src, sizeof(resolved_src));
        struct stat src_stat;
        if (stat(src, &src_stat) != 0) {
            fprintf(stderr, "cp: %s: %s\n", src, strerror(errno));
            status = 1;
            continue;
        }
        if (!S_ISREG(src_stat.st_mode)) {
            fprintf(stderr, "cp: %s: unsupported file type\n", src);
            status = 1;
            continue;
        }
        char target_path[PATH_MAX];
        char resolved_dest[PATH_MAX];
        const char *target = smallclueResolvePath(dest, resolved_dest, sizeof(resolved_dest));
        if (dest_is_dir) {
            if (smallclueBuildPath(target_path, sizeof(target_path), dest_real, smallclueLeafName(src)) != 0) {
                fprintf(stderr, "cp: %s/%s: %s\n", dest, smallclueLeafName(src), strerror(errno));
                status = 1;
                continue;
            }
            target = target_path;
        }
        struct stat target_stat;
        if (stat(target, &target_stat) == 0) {
            if (target_stat.st_dev == src_stat.st_dev && target_stat.st_ino == src_stat.st_ino) {
                fprintf(stderr, "cp: '%s' and '%s' are the same file\n", src, target);
                status = 1;
                continue;
            }
        }
        if (smallclueCopyFile("cp", src, target) != 0) {
            status = 1;
        }
    }
    return status;
}

static int smallclueMvCommand(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "mv: missing file operand\n");
        return 1;
    }
    const char *dest = argv[argc - 1];
    char resolved_dest_root[PATH_MAX];
    const char *dest_real = smallclueResolvePath(dest, resolved_dest_root, sizeof(resolved_dest_root));
    struct stat dest_stat;
    int dest_exists = (stat(dest_real, &dest_stat) == 0);
    bool dest_is_dir = dest_exists && S_ISDIR(dest_stat.st_mode);
    int source_count = argc - 2;
    if (source_count > 1 && !dest_is_dir) {
        fprintf(stderr, "mv: target '%s' is not a directory\n", dest);
        return 1;
    }
    int status = 0;
    for (int i = 1; i <= source_count; ++i) {
        char resolved_src[PATH_MAX];
        const char *src = smallclueResolvePath(argv[i], resolved_src, sizeof(resolved_src));
        char target_path[PATH_MAX];
        char resolved_dest_dir[PATH_MAX];
        const char *dest_base = dest;
        if (!dest_is_dir) {
            dest_base = smallclueResolvePath(dest, resolved_dest_dir, sizeof(resolved_dest_dir));
        }
        const char *target = dest_base;
        if (dest_is_dir) {
            const char *dir_root = smallclueResolvePath(dest, resolved_dest_dir, sizeof(resolved_dest_dir));
            if (smallclueBuildPath(target_path, sizeof(target_path), dir_root, smallclueLeafName(src)) != 0) {
                fprintf(stderr, "mv: %s/%s: %s\n", dir_root, smallclueLeafName(src), strerror(errno));
                status = 1;
                continue;
            }
            target = target_path;
        }
        if (rename(src, target) == 0) {
            continue;
        }
        if (errno == EXDEV) {
            if (smallclueCopyFile("mv", src, target) != 0) {
                status = 1;
                continue;
            }
            if (smallclueRemovePathWithLabel("mv", src, false, true) != 0) {
                fprintf(stderr, "mv: %s: unable to remove after copy\n", src);
                status = 1;
            }
        } else {
            fprintf(stderr, "mv: %s -> %s: %s\n", src, target, strerror(errno));
            status = 1;
        }
    }
    return status;
}

const SmallclueApplet *smallclueGetApplets(size_t *count) {
    if (count) {
        *count = kSmallclueAppletCount;
    }
    return kSmallclueApplets;
}

const SmallclueApplet *smallclueFindApplet(const char *name) {
    if (!name || !*name) {
        return NULL;
    }
    for (size_t i = 0; i < kSmallclueAppletCount; ++i) {
        if (strcasecmp(kSmallclueApplets[i].name, name) == 0) {
            return &kSmallclueApplets[i];
        }
    }
    return NULL;
}

int smallclueDispatchApplet(const SmallclueApplet *applet, int argc, char **argv) {
    if (!applet || !applet->entry) {
        return 127;
    }
    optind = 1;
    return applet->entry(argc, argv);
}

int smallclueMain(int argc, char **argv) {
    const SmallclueApplet *applet = NULL;
    char *call_name = basename(argv[0]);

    if (strcmp(call_name, "smallclue") == 0 || strcmp(call_name, "smallclu") == 0) {
        if (argc < 2) {
            print_usage();
            return 1;
        }
        call_name = argv[1];
        argv++;
        argc--;
    }

    applet = smallclueFindApplet(call_name);
    if (!applet) {
        fprintf(stderr, "smallclue: '%s' applet not found.\n\n", call_name);
        print_usage();
        return 127;
    }

    return smallclueDispatchApplet(applet, argc, argv);
}
static bool smallclueIsInteger(const char *s, long long *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long long value = strtoll(s, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (out) {
        *out = value;
    }
    return true;
}

static bool smallclueTestEvaluate(int argc, char **argv) {
    if (argc <= 0) {
        return false;
    }
    if (strcmp(argv[0], "!") == 0) {
        return !smallclueTestEvaluate(argc - 1, argv + 1);
    }
    if (argc == 1) {
        return argv[0][0] != '\0';
    }
    if (argc == 2) {
        const char *op = argv[0];
        const char *arg = argv[1];
        if (strcmp(op, "-z") == 0) {
            return arg[0] == '\0';
        }
        if (strcmp(op, "-n") == 0) {
            return arg[0] != '\0';
        }
        if (strcmp(op, "-e") == 0) {
            return access(arg, F_OK) == 0;
        }
        if (strcmp(op, "-f") == 0) {
            struct stat st;
            return stat(arg, &st) == 0 && S_ISREG(st.st_mode);
        }
        if (strcmp(op, "-d") == 0) {
            struct stat st;
            return stat(arg, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (strcmp(op, "-r") == 0) {
            return access(arg, R_OK) == 0;
        }
        if (strcmp(op, "-w") == 0) {
            return access(arg, W_OK) == 0;
        }
    }
    if (argc == 3) {
        const char *left = argv[0];
        const char *op = argv[1];
        const char *right = argv[2];
        if (strcmp(op, "=") == 0) {
            return strcmp(left, right) == 0;
        }
        if (strcmp(op, "!=") == 0) {
            return strcmp(left, right) != 0;
        }
        long long lhs, rhs;
        if (smallclueIsInteger(left, &lhs) && smallclueIsInteger(right, &rhs)) {
            if (strcmp(op, "-eq") == 0) return lhs == rhs;
            if (strcmp(op, "-ne") == 0) return lhs != rhs;
            if (strcmp(op, "-gt") == 0) return lhs > rhs;
            if (strcmp(op, "-ge") == 0) return lhs >= rhs;
            if (strcmp(op, "-lt") == 0) return lhs < rhs;
            if (strcmp(op, "-le") == 0) return lhs <= rhs;
        }
    }
    fprintf(stderr, "test: unsupported expression\n");
    return false;
}

static int smallclueTestWithArgs(int argc, char **argv) {
    if (argc <= 0) {
        return 1;
    }
    bool result = smallclueTestEvaluate(argc, argv);
    return result ? 0 : 1;
}

static int smallclueTestCommand(int argc, char **argv) {
    return smallclueTestWithArgs(argc - 1, argv + 1);
}

static int smallclueBracketCommand(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
        fprintf(stderr, "[: missing ']'\n");
        return 1;
    }
    return smallclueTestWithArgs(argc - 2, argv + 1);
}

static int smallcluePbcopyCommand(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        fprintf(stderr, "pbcopy: takes no arguments\n");
        return 1;
    }
    smallclueResetGetopt();
    char buf[4096];
    size_t total = 0;
    size_t cap = 4096;
    char *data = (char *)malloc(cap);
    if (!data) {
        fprintf(stderr, "pbcopy: out of memory\n");
        return 1;
    }
    ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        if (total + (size_t)n > cap) {
            size_t newcap = cap * 2;
            while (newcap < total + (size_t)n) newcap *= 2;
            char *tmp = (char *)realloc(data, newcap);
            if (!tmp) {
                free(data);
                fprintf(stderr, "pbcopy: out of memory\n");
                return 1;
            }
            data = tmp;
            cap = newcap;
        }
        memcpy(data + total, buf, (size_t)n);
        total += (size_t)n;
    }
    int rc = runtimeClipboardSet(data, total);
    free(data);
    if (rc != 0) {
        fprintf(stderr, "pbcopy: clipboard unavailable\n");
        return 1;
    }
    return 0;
}

static int smallcluePbpasteCommand(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        fprintf(stderr, "pbpaste: takes no arguments\n");
        return 1;
    }
    smallclueResetGetopt();
    size_t len = 0;
    char *text = runtimeClipboardGet(&len);
    if (!text) {
        fprintf(stderr, "pbpaste: clipboard unavailable\n");
        return 1;
    }
    ssize_t written = write(STDOUT_FILENO, text, len);
    free(text);
    return written < 0 ? 1 : 0;
}
