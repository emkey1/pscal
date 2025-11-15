#include "smallclu/smallclu.h"

#include "common/runtime_tty.h"
#include "smallclu/elvis_app.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <libgen.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

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

static int smallcluEchoCommand(int argc, char **argv);
static int smallcluLsCommand(int argc, char **argv);
static int smallcluEditorCommand(int argc, char **argv);
static int smallcluCatCommand(int argc, char **argv);
static int smallcluPagerCommand(int argc, char **argv);
static int smallcluClearCommand(int argc, char **argv);
#if defined(PSCAL_TARGET_IOS)
static int smallcluElvisCommand(int argc, char **argv);
#endif

static const SmallcluApplet kSmallcluApplets[] = {
    {"cat", smallcluCatCommand, "Concatenate files"},
    {"clear", smallcluClearCommand, "Clear the terminal"},
    {"cls", smallcluClearCommand, "Clear the terminal"},
    {"echo", smallcluEchoCommand, "Print arguments"},
    {"editor", smallcluEditorCommand, "Minimal raw-mode editor"},
    {"less", smallcluPagerCommand, "Paginate file contents"},
    {"ls", smallcluLsCommand, "List directory contents"},
    {"more", smallcluPagerCommand, "Paginate file contents"},
#if defined(PSCAL_TARGET_IOS)
    {"elvis", smallcluElvisCommand, "Elvis text editor"},
#endif
};

static size_t kSmallcluAppletCount = sizeof(kSmallcluApplets) / sizeof(kSmallcluApplets[0]);

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

static int pager_control_fd(void) {
    if (pager_control_fd_value != -2) {
        return pager_control_fd_value;
    }
#ifdef _WIN32
    pager_control_fd_value = -1;
#else
    int fd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
    if (fd < 0 && pscalRuntimeStdinIsInteractive()) {
        fd = dup(STDIN_FILENO);
    }
    pager_control_fd_value = fd;
#endif
    return pager_control_fd_value;
}

static int pager_read_key(void) {
    int fd = pager_control_fd();
    if (fd < 0) {
        return EOF;
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
    int result = EOF;
    unsigned char ch = 0;
    ssize_t n = read(fd, &ch, 1);
    if (n > 0) {
        if (ch == '\x1b') {
            unsigned char seq[3] = {0};
            if (read(fd, &seq[0], 1) == 1) {
                if (seq[0] == '[') {
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
                result = '\x1b';
            }
            if (result == EOF) {
                result = '\x1b';
            }
        } else {
            result = ch;
        }
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
    if (!pscalRuntimeStdoutIsInteractive()) {
        return print_file(path, stream);
    }

    PagerBuffer buffer = {0};
    if (pagerCollectLines(cmd_name, path, stream, &buffer) != 0) {
        return 1;
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

static void print_long_listing(const char *filename, const struct stat *s) {
    print_permissions(s->st_mode);
    printf(" %2llu", (unsigned long long)s->st_nlink);

    struct passwd *pw = getpwuid(s->st_uid);
    printf(" %-8s", pw ? pw->pw_name : "?");

    struct group *gr = getgrgid(s->st_gid);
    printf(" %-8s", gr ? gr->gr_name : "?");

    printf(" %8lld", (long long)s->st_size);
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

static int print_path_entry(const char *path, const char *label, bool long_format) {
    struct stat stat_buf;
    if (lstat(path, &stat_buf) == -1) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (long_format) {
        print_long_listing(label ? label : path, &stat_buf);
    } else {
        printf("%s\n", label ? label : path);
    }
    return 0;
}

static int list_directory(const char *path, bool show_all, bool long_format) {
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    struct dirent *dir;
    int status = 0;
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
        status |= print_path_entry(full_path, filename, long_format);
        free(full_path);
    }
    closedir(d);
    return status ? 1 : 0;
}

static void editor_disable_raw_mode(void);
static struct termios g_editor_orig_termios;

static void editor_die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

static void editor_disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_editor_orig_termios);
}

static void editor_enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &g_editor_orig_termios) == -1) {
        editor_die("tcgetattr");
    }
    atexit(editor_disable_raw_mode);
    struct termios raw = g_editor_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        editor_die("tcsetattr");
    }
}

static void print_usage(void) {
    fprintf(stderr, "This is smallclu. Usage:\n");
    fprintf(stderr, "  smallclu <applet> [arguments...]\n\n");
    fprintf(stderr, "Available applets:\n");
    for (size_t i = 0; i < kSmallcluAppletCount; ++i) {
        const SmallcluApplet *applet = &kSmallcluApplets[i];
        fprintf(stderr, "  %-8s %s\n", applet->name, applet->description ? applet->description : "");
    }
    fprintf(stderr, "\nYou can symlink applets to 'smallclu' or invoke them directly.\n");
}

static int smallcluEchoCommand(int argc, char **argv) {
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

static int smallcluLsCommand(int argc, char **argv) {
    int show_all = 0;
    int long_format = 0;
    int opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "al")) != -1) {
        switch (opt) {
            case 'a':
                show_all = 1;
                break;
            case 'l':
                long_format = 1;
                break;
            default:
                return 1;
        }
    }

    int status = 0;
    int paths_start = optind;
    if (paths_start >= argc) {
        return list_directory(".", show_all, long_format);
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
        if (is_dir) {
            if (remaining > 1) {
                if (i > paths_start) {
                    putchar('\n');
                }
                printf("%s:\n", path);
            }
            status |= list_directory(path, show_all, long_format);
        } else {
            status |= print_path_entry(path, path, long_format);
        }
    }
    return status ? 1 : 0;
}

static int smallcluEditorCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    editor_enable_raw_mode();
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    const char *msg = "smallclu-editor -- Press 'q' to quit.\r\n";
    write(STDOUT_FILENO, msg, strlen(msg));
    while (1) {
        char c = '\0';
        if (read(STDIN_FILENO, &c, 1) == -1) {
            editor_die("read");
        }
        if (c == 'q') {
            break;
        } else if (iscntrl((unsigned char)c)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "(%d)\r\n", c);
            write(STDOUT_FILENO, buf, strlen(buf));
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%c\r\n", c);
            write(STDOUT_FILENO, buf, strlen(buf));
        }
    }
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    return 0;
}

#if defined(PSCAL_TARGET_IOS)
static int smallcluElvisCommand(int argc, char **argv) {
    return smallcluRunElvis(argc, argv);
}
#else
static int smallcluElvisCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "elvis: not supported on this platform\n");
    return 127;
}
#endif

static int smallcluCatCommand(int argc, char **argv) {
    int status = 0;
    if (argc <= 1) {
        return cat_file(NULL);
    }
    for (int i = 1; i < argc; ++i) {
        status |= cat_file(argv[i]);
    }
    return status ? 1 : 0;
}

static int smallcluPagerCommand(int argc, char **argv) {
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

static int smallcluClearCommand(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fputs("\x1b[2J\x1b[H", stdout);
    fflush(stdout);
    return 0;
}

const SmallcluApplet *smallcluGetApplets(size_t *count) {
    if (count) {
        *count = kSmallcluAppletCount;
    }
    return kSmallcluApplets;
}

const SmallcluApplet *smallcluFindApplet(const char *name) {
    if (!name || !*name) {
        return NULL;
    }
    for (size_t i = 0; i < kSmallcluAppletCount; ++i) {
        if (strcasecmp(kSmallcluApplets[i].name, name) == 0) {
            return &kSmallcluApplets[i];
        }
    }
    return NULL;
}

int smallcluDispatchApplet(const SmallcluApplet *applet, int argc, char **argv) {
    if (!applet || !applet->entry) {
        return 127;
    }
    optind = 1;
    return applet->entry(argc, argv);
}

int smallcluMain(int argc, char **argv) {
    const SmallcluApplet *applet = NULL;
    char *call_name = basename(argv[0]);

    if (strcmp(call_name, "smallclu") == 0) {
        if (argc < 2) {
            print_usage();
            return 1;
        }
        call_name = argv[1];
        argv++;
        argc--;
    }

    applet = smallcluFindApplet(call_name);
    if (!applet) {
        fprintf(stderr, "smallclu: '%s' applet not found.\n\n", call_name);
        print_usage();
        return 127;
    }

    return smallcluDispatchApplet(applet, argc, argv);
}
