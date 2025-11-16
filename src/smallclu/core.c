#include "smallclu/smallclu.h"

#include "common/runtime_tty.h"
#include "smallclu/elvis_app.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <libgen.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
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

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} SmallcluLineVector;

static int smallcluEchoCommand(int argc, char **argv);
static int smallcluLsCommand(int argc, char **argv);
static int smallcluEditorCommand(int argc, char **argv);
static int smallcluCatCommand(int argc, char **argv);
static int smallcluPagerCommand(int argc, char **argv);
static int smallcluClearCommand(int argc, char **argv);
static int smallcluRmCommand(int argc, char **argv);
static int smallcluCpCommand(int argc, char **argv);
static int smallcluMvCommand(int argc, char **argv);
static int smallcluDateCommand(int argc, char **argv);
static int smallcluCalCommand(int argc, char **argv);
static int smallcluHeadCommand(int argc, char **argv);
static int smallcluGrepCommand(int argc, char **argv);
static int smallcluWcCommand(int argc, char **argv);
static int smallcluDuCommand(int argc, char **argv);
static int smallcluFindCommand(int argc, char **argv);
static int smallcluTailCommand(int argc, char **argv);
static int smallcluTouchCommand(int argc, char **argv);
static int smallcluSttyCommand(int argc, char **argv);
static int smallcluResizeCommand(int argc, char **argv);
static int smallcluSortCommand(int argc, char **argv);
static int smallcluUniqCommand(int argc, char **argv);
static int smallcluSedCommand(int argc, char **argv);
static int smallcluCutCommand(int argc, char **argv);
static int smallcluTrCommand(int argc, char **argv);
static int smallcluIdCommand(int argc, char **argv);
static int smallcluMkdirCommand(int argc, char **argv);
static int smallcluLnCommand(int argc, char **argv);
static int smallcluTypeCommand(int argc, char **argv);
static int smallcluFileCommand(int argc, char **argv);
static const char *smallcluLeafName(const char *path);
static int smallcluBuildPath(char *buf, size_t buf_size, const char *dir, const char *leaf);
static int smallcluRemovePathWithLabel(const char *label, const char *path, bool recursive);
static int smallcluCopyFile(const char *label, const char *src, const char *dst);
static int smallcluMkdirParents(const char *path, mode_t mode);
#if defined(PSCAL_TARGET_IOS)
static int smallcluElvisCommand(int argc, char **argv);
#endif

static const SmallcluApplet kSmallcluApplets[] = {
    {"cal", smallcluCalCommand, "Show a simple calendar"},
    {"cat", smallcluCatCommand, "Concatenate files"},
    {"clear", smallcluClearCommand, "Clear the terminal"},
    {"cls", smallcluClearCommand, "Clear the terminal"},
    {"cp", smallcluCpCommand, "Copy files"},
    {"cut", smallcluCutCommand, "Extract fields from lines"},
    {"date", smallcluDateCommand, "Display current date/time"},
    {"du", smallcluDuCommand, "Summarize disk usage"},
    {"echo", smallcluEchoCommand, "Print arguments"},
    {"editor", smallcluEditorCommand, "Minimal raw-mode editor"},
#if defined(PSCAL_TARGET_IOS)
    {"elvis", smallcluElvisCommand, "Elvis text editor"},
#endif
    {"file", smallcluFileCommand, "Identify file types"},
    {"find", smallcluFindCommand, "Search for files"},
    {"grep", smallcluGrepCommand, "Search for patterns"},
    {"head", smallcluHeadCommand, "Print the first lines of files"},
    {"id", smallcluIdCommand, "Print user identity information"},
    {"less", smallcluPagerCommand, "Paginate file contents"},
    {"ln", smallcluLnCommand, "Create links"},
    {"ls", smallcluLsCommand, "List directory contents"},
    {"mkdir", smallcluMkdirCommand, "Create directories"},
    {"more", smallcluPagerCommand, "Paginate file contents"},
    {"mv", smallcluMvCommand, "Move or rename files"},
    {"resize", smallcluResizeCommand, "Synchronize terminal rows/columns"},
    {"rm", smallcluRmCommand, "Remove files"},
    {"sed", smallcluSedCommand, "Stream editor for simple substitutions"},
    {"sort", smallcluSortCommand, "Sort lines of text"},
    {"stty", smallcluSttyCommand, "Adjust terminal rows/columns"},
    {"tail", smallcluTailCommand, "Print the last lines of files"},
    {"touch", smallcluTouchCommand, "Update file timestamps"},
    {"tr", smallcluTrCommand, "Translate or delete characters"},
    {"type", smallcluTypeCommand, "Describe command names"},
    {"uniq", smallcluUniqCommand, "Report or omit repeated lines"},
    {"wc", smallcluWcCommand, "Count lines/words/bytes"},
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

static bool smallcluLineVectorAppend(SmallcluLineVector *vec, const char *data, size_t len) {
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

static void smallcluLineVectorFree(SmallcluLineVector *vec) {
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

static int smallcluLineVectorLoadStream(FILE *fp, const char *path, const char *cmd_name, SmallcluLineVector *vec) {
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
        if (!smallcluLineVectorAppend(vec, line, (size_t)len)) {
            fprintf(stderr, "%s: %s: out of memory\n", cmd_name, path ? path : "(stdin)");
            status = 1;
            break;
        }
    }
    free(line);
    return status;
}

static int smallcluStringCompare(const void *a, const void *b) {
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

static int smallcluDateCommand(int argc, char **argv) {
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

static bool smallcluParseInt(const char *text, int min, int max, int *out_value) {
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

static bool smallcluIsLeapYear(int year) {
    return ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0));
}

static int smallcluDaysInMonth(int month, int year) {
    static const int days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 30;
    }
    if (month == 2 && smallcluIsLeapYear(year)) {
        return 29;
    }
    return days_per_month[month - 1];
}

static int smallcluFirstWeekdayOfMonth(int month, int year) {
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

static int smallcluCalCommand(int argc, char **argv) {
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
        if (!smallcluParseInt(argv[1], 1, 12, &month) || !smallcluParseInt(argv[2], 1, 9999, &year)) {
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

    int first_wday = smallcluFirstWeekdayOfMonth(month, year);
    int days = smallcluDaysInMonth(month, year);
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

static const char *smallcluStrCaseStr(const char *haystack, const char *needle, int ignore_case) {
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

static bool smallcluParseDashLineCount(const char *arg, long *value) {
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

static int smallcluHeadStream(FILE *fp, const char *label, long lines) {
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

static int smallcluHeadCommand(int argc, char **argv) {
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
        if (smallcluParseDashLineCount(arg, &dashLines)) {
            lines = dashLines;
            index += 1;
            continue;
        }
        fprintf(stderr, "head: unsupported option '%s'\n", arg);
        return 1;
    }

    int status = 0;
    if (index >= argc) {
        status = smallcluHeadStream(stdin, "(stdin)", lines);
    } else {
        for (int i = index; i < argc; ++i) {
            const char *path = argv[i];
            FILE *fp = fopen(path, "r");
            if (!fp) {
                fprintf(stderr, "head: %s: %s\n", path, strerror(errno));
                status = 1;
                continue;
            }
            status |= smallcluHeadStream(fp, path, lines);
            fclose(fp);
        }
    }
    return status ? 1 : 0;
}

static int smallcluTailStream(FILE *fp, const char *label, long lines) {
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

static int smallcluTailCommand(int argc, char **argv) {
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
        if (smallcluParseDashLineCount(arg, &dashLines)) {
            lines = dashLines;
            index += 1;
            continue;
        }
        fprintf(stderr, "tail: unsupported option '%s'\n", arg);
        return 1;
    }
    int status = 0;
    if (index >= argc) {
        status = smallcluTailStream(stdin, "(stdin)", lines);
    } else {
        for (int i = index; i < argc; ++i) {
            const char *path = argv[i];
            FILE *fp = fopen(path, "r");
            if (!fp) {
                fprintf(stderr, "tail: %s: %s\n", path, strerror(errno));
                status = 1;
                continue;
            }
            status |= smallcluTailStream(fp, path, lines);
            fclose(fp);
        }
    }
    return status ? 1 : 0;
}

static int smallcluTouchCommand(int argc, char **argv) {
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
        int fd = open(path, O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            fprintf(stderr, "touch: %s: %s\n", path, strerror(errno));
            status = 1;
            continue;
        }
        close(fd);
        if (utimes(path, times) != 0) {
            fprintf(stderr, "touch: %s: %s\n", path, strerror(errno));
            status = 1;
        }
    }
    return status ? 1 : 0;
}

static long smallcluParseLong(const char *text) {
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

static void smallcluEmitTerminalReset(void) {
    fputs("\x1b" "c", stdout); // RIS: full reset
    fflush(stdout);
}

static void smallcluEmitTerminalSane(void) {
    fputs("\x1b[0m\x1b[?7h\x1b[?25h", stdout); // reset attributes, enable wrap & cursor
    fflush(stdout);
}

static void smallcluApplyWindowSize(int rows, int cols) {
    if (rows > 0 && cols > 0) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", rows);
        setenv("LINES", buffer, 1);
        snprintf(buffer, sizeof(buffer), "%d", cols);
        setenv("COLUMNS", buffer, 1);
        printf("\x1b[8;%d;%dt", rows, cols);
        fflush(stdout);
    }
}

static int smallcluSttyCommand(int argc, char **argv) {
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
            rows = smallcluParseLong(argv[index + 1]);
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
            cols = smallcluParseLong(argv[index + 1]);
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
            rows = smallcluParseLong(argv[index + 1]);
            cols = smallcluParseLong(argv[index + 2]);
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
        smallcluEmitTerminalReset();
    }
    if (requestSane) {
        smallcluEmitTerminalSane();
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
        rows = env ? smallcluParseLong(env) : 24;
        if (rows <= 0) rows = 24;
    }
    if (cols <= 0) {
        const char *env = getenv("COLUMNS");
        cols = env ? smallcluParseLong(env) : 80;
        if (cols <= 0) cols = 80;
    }

    smallcluApplyWindowSize((int)rows, (int)cols);
    return 0;
}

static int smallcluResizeCommand(int argc, char **argv) {
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
    smallcluApplyWindowSize(rows, cols);
    return 0;
}

static int smallcluSortCommand(int argc, char **argv) {
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

    SmallcluLineVector vec = {0};
    int status = 0;
    if (index >= argc) {
        status = smallcluLineVectorLoadStream(stdin, NULL, "sort", &vec);
    } else {
        for (int i = index; i < argc && status == 0; ++i) {
            FILE *fp = fopen(argv[i], "r");
            if (!fp) {
                fprintf(stderr, "sort: %s: %s\n", argv[i], strerror(errno));
                status = 1;
                break;
            }
            status = smallcluLineVectorLoadStream(fp, argv[i], "sort", &vec);
            fclose(fp);
        }
    }
    if (status == 0 && vec.count > 1) {
        qsort(vec.items, vec.count, sizeof(char *), smallcluStringCompare);
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
    smallcluLineVectorFree(&vec);
    return status;
}

static int smallcluUniqStream(FILE *fp, const char *path, int print_counts) {
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

static int smallcluUniqCommand(int argc, char **argv) {
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
        return smallcluUniqStream(stdin, "(stdin)", print_counts);
    }
    int status = 0;
    for (int i = index; i < argc; ++i) {
        FILE *fp = fopen(argv[i], "r");
        if (!fp) {
            fprintf(stderr, "uniq: %s: %s\n", argv[i], strerror(errno));
            status = 1;
            continue;
        }
        status |= smallcluUniqStream(fp, argv[i], print_counts);
        fclose(fp);
    }
    return status;
}

static bool smallcluSedParseExpr(const char *expr, char **pattern, char **replacement, bool *global) {
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

static char *smallcluSedApply(const char *line, const char *pattern, const char *replacement, bool global) {
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

static int smallcluSedCommand(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "sed: missing expression\n");
        return 1;
    }
    char *pattern = NULL;
    char *replacement = NULL;
    bool global = false;
    if (!smallcluSedParseExpr(argv[1], &pattern, &replacement, &global)) {
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
            char *out = smallcluSedApply(line, pattern, replacement, global);
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
                char *out = smallcluSedApply(line, pattern, replacement, global);
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

static void smallcluCutPrintField(const char *line, char delim, int field) {
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

static int smallcluCutCommand(int argc, char **argv) {
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
            field = (int)smallcluParseLong(argv[index + 1]);
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
            smallcluCutPrintField(line, delimiter, field);
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
                smallcluCutPrintField(line, delimiter, field);
            }
            fclose(fp);
        }
    }
    free(line);
    return status;
}

static int smallcluTrCommand(int argc, char **argv) {
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

static int smallcluIdCommand(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        fprintf(stderr, "id: no user lookup support in smallclu\n");
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

static int smallcluGrepCommand(int argc, char **argv) {
    int index = 1;
    int number_lines = 0;
    int ignore_case = 0;
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
            if (*opt == 'n') {
                number_lines = 1;
            } else if (*opt == 'i') {
                ignore_case = 1;
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
            if (smallcluStrCaseStr(line, pattern, ignore_case) != NULL) {
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
                if (smallcluStrCaseStr(line, pattern, ignore_case) != NULL) {
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
    long lines;
    long words;
    long bytes;
} SmallcluWcCounts;

static int smallcluWcProcessFile(const char *path, SmallcluWcCounts *counts) {
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

static void smallcluWcPrint(const SmallcluWcCounts *counts, int show_lines, int show_words, int show_bytes, const char *label) {
    if (show_lines) {
        printf("%7ld", counts->lines);
    }
    if (show_words) {
        printf("%7ld", counts->words);
    }
    if (show_bytes) {
        printf("%7ld", counts->bytes);
    }
    if (label) {
        printf(" %s", label);
    }
    putchar('\n');
}

static int smallcluWcCommand(int argc, char **argv) {
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
    SmallcluWcCounts counts;
    SmallcluWcCounts total = {0, 0, 0};
    if (paths <= 0) {
        if (smallcluWcProcessFile(NULL, &counts) != 0) {
            return 1;
        }
        smallcluWcPrint(&counts, show_lines, show_words, show_bytes, NULL);
    } else {
        for (int i = index; i < argc; ++i) {
            if (smallcluWcProcessFile(argv[i], &counts) != 0) {
                status = 1;
                continue;
            }
            smallcluWcPrint(&counts, show_lines, show_words, show_bytes, argv[i]);
            total.lines += counts.lines;
            total.words += counts.words;
            total.bytes += counts.bytes;
        }
        if (paths > 1) {
            smallcluWcPrint(&total, show_lines, show_words, show_bytes, "total");
        }
    }
    return status;
}

static long long smallcluDuVisit(const char *path, int *status) {
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
            if (smallcluBuildPath(child, sizeof(child), path, entry->d_name) != 0) {
                fprintf(stderr, "du: %s/%s: %s\n", path, entry->d_name, strerror(errno));
                if (status) *status = 1;
                continue;
            }
            total += smallcluDuVisit(child, status);
        }
        closedir(dir);
    }
    printf("%lld\t%s\n", total, path);
    return total;
}

static int smallcluDuCommand(int argc, char **argv) {
    int status = 0;
    if (argc <= 1) {
        smallcluDuVisit(".", &status);
    } else {
        for (int i = 1; i < argc; ++i) {
            smallcluDuVisit(argv[i], &status);
        }
    }
    return status ? 1 : 0;
}

static int smallcluFindVisit(const char *path, const char *pattern, int *status) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "find: %s: %s\n", path, strerror(errno));
        if (status) *status = 1;
        return 1;
    }
    const char *leaf = smallcluLeafName(path);
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
            if (smallcluBuildPath(child, sizeof(child), path, entry->d_name) != 0) {
                fprintf(stderr, "find: %s/%s: %s\n", path, entry->d_name, strerror(errno));
                if (status) *status = 1;
                continue;
            }
            smallcluFindVisit(child, pattern, status);
        }
        closedir(dir);
    }
    return 0;
}

static int smallcluFindCommand(int argc, char **argv) {
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
    smallcluFindVisit(start, pattern, &status);
    return status ? 1 : 0;
}

static const char *smallcluLeafName(const char *path) {
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

static int smallcluBuildPath(char *buf, size_t buf_size, const char *dir, const char *leaf) {
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

static int smallcluRemovePathWithLabel(const char *label, const char *path, bool recursive) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "%s: %s: %s\n", label, path, strerror(errno));
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "%s: %s: is a directory\n", label, path);
            return -1;
        }
        DIR *dir = opendir(path);
        if (!dir) {
            fprintf(stderr, "%s: %s: %s\n", label, path, strerror(errno));
            return -1;
        }
        struct dirent *entry;
        int status = 0;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char child_path[PATH_MAX];
            if (smallcluBuildPath(child_path, sizeof(child_path), path, entry->d_name) != 0) {
                fprintf(stderr, "%s: %s/%s: %s\n", label, path, entry->d_name, strerror(errno));
                status = -1;
                break;
            }
            if (smallcluRemovePathWithLabel(label, child_path, true) != 0) {
                status = -1;
            }
        }
        closedir(dir);
        if (status != 0) {
            return -1;
        }
        if (rmdir(path) != 0) {
            fprintf(stderr, "%s: %s: %s\n", label, path, strerror(errno));
            return -1;
        }
        return 0;
    }
    if (unlink(path) != 0) {
        fprintf(stderr, "%s: %s: %s\n", label, path, strerror(errno));
        return -1;
    }
    return 0;
}

static int smallcluCopyFile(const char *label, const char *src, const char *dst) {
    int in_fd = open(src, O_RDONLY);
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
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
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

static int smallcluMkdirParents(const char *path, mode_t mode) {
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

static int smallcluRmCommand(int argc, char **argv) {
    int recursive = 0;
    int opt;
    optind = 1;
    while ((opt = getopt(argc, argv, "r")) != -1) {
        switch (opt) {
            case 'r':
                recursive = 1;
                break;
            default:
                fprintf(stderr, "rm: invalid option -- %c\n", optopt);
                return 1;
        }
    }
    if (optind >= argc) {
        fprintf(stderr, "rm: missing operand\n");
        return 1;
    }
    int status = 0;
    for (int i = optind; i < argc; ++i) {
        if (smallcluRemovePathWithLabel("rm", argv[i], recursive != 0) != 0) {
            status = 1;
        }
    }
    return status;
}

static int smallcluMkdirCommand(int argc, char **argv) {
    int parents = 0;
    int opt;
    optind = 1;
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
            if (smallcluMkdirParents(target, 0777) != 0) {
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

static int smallcluFileCommand(int argc, char **argv) {
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

static int smallcluLnCommand(int argc, char **argv) {
    int symbolic = 0;
    int opt;
    optind = 1;
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

static char *smallcluSearchPath(const char *name) {
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

static int smallcluTypeCommand(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "type: missing operand\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; ++i) {
        const char *name = argv[i];
        const SmallcluApplet *applet = smallcluFindApplet(name);
        if (applet) {
            printf("%s is a smallclu applet\n", name);
            continue;
        }
        char *path = smallcluSearchPath(name);
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

static int smallcluCpCommand(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "cp: missing file operand\n");
        return 1;
    }
    const char *dest = argv[argc - 1];
    struct stat dest_stat;
    int dest_exists = (stat(dest, &dest_stat) == 0);
    bool dest_is_dir = dest_exists && S_ISDIR(dest_stat.st_mode);
    int source_count = argc - 2;
    if (source_count > 1 && !dest_is_dir) {
        fprintf(stderr, "cp: target '%s' is not a directory\n", dest);
        return 1;
    }
    int status = 0;
    for (int i = 1; i <= source_count; ++i) {
        const char *src = argv[i];
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
        const char *target = dest;
        if (dest_is_dir) {
            if (smallcluBuildPath(target_path, sizeof(target_path), dest, smallcluLeafName(src)) != 0) {
                fprintf(stderr, "cp: %s/%s: %s\n", dest, smallcluLeafName(src), strerror(errno));
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
        if (smallcluCopyFile("cp", src, target) != 0) {
            status = 1;
        }
    }
    return status;
}

static int smallcluMvCommand(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "mv: missing file operand\n");
        return 1;
    }
    const char *dest = argv[argc - 1];
    struct stat dest_stat;
    int dest_exists = (stat(dest, &dest_stat) == 0);
    bool dest_is_dir = dest_exists && S_ISDIR(dest_stat.st_mode);
    int source_count = argc - 2;
    if (source_count > 1 && !dest_is_dir) {
        fprintf(stderr, "mv: target '%s' is not a directory\n", dest);
        return 1;
    }
    int status = 0;
    for (int i = 1; i <= source_count; ++i) {
        const char *src = argv[i];
        char target_path[PATH_MAX];
        const char *target = dest;
        if (dest_is_dir) {
            if (smallcluBuildPath(target_path, sizeof(target_path), dest, smallcluLeafName(src)) != 0) {
                fprintf(stderr, "mv: %s/%s: %s\n", dest, smallcluLeafName(src), strerror(errno));
                status = 1;
                continue;
            }
            target = target_path;
        }
        if (rename(src, target) == 0) {
            continue;
        }
        if (errno == EXDEV) {
            if (smallcluCopyFile("mv", src, target) != 0) {
                status = 1;
                continue;
            }
            if (smallcluRemovePathWithLabel("mv", src, false) != 0) {
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
