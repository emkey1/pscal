#include <stdio.h>
#include <string.h>
#include <libgen.h>   // For basename()
#include <unistd.h>   // For chdir(), write(), read(), STDIN_FILENO, getopt()
#include <dirent.h>   // For opendir(), readdir(), closedir()
#include <stdlib.h>   // For atexit(), exit()
#include <termios.h>  // For terminal raw mode
#include <ctype.h>    // For iscntrl()
#include <sys/stat.h> // For stat(), lstat()
#include <sys/types.h>
#include <pwd.h>      // For getpwuid()
#include <grp.h>      // For getgrgid()
#include <time.h>     // For strftime()

/*
 * ==================================================================
 * Applet Implementation: echo (Upgraded)
 * ==================================================================
 *
 * Implements 'echo' with support for the '-n' flag.
 */
int echo_main(int argc, char *argv[]) {
    int print_newline = 1; // Flag to control the newline
    int start_index = 1;   // First argument to print

    // Check for the -n flag
    // Note: A real 'echo' has complex flag parsing. We'll just check
    // if the first argument is *exactly* "-n".
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        print_newline = 0;
        start_index = 2; // Start printing from the next argument
    }

    // Loop from the first word to print
    for (int i = start_index; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) {
            printf(" ");
        }
    }
    
    if (print_newline) {
        printf("\n");
    }

    return 0;
}

/*
 * ==================================================================
 * Applet Implementation: ls (Upgraded)
 * ==================================================================
 */

// Helper function to print file permissions for 'ls -l'
void print_permissions(mode_t mode) {
    // File type
    putchar(S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-');
    
    // User permissions
    putchar(mode & S_IRUSR ? 'r' : '-');
    putchar(mode & S_IWUSR ? 'w' : '-');
    putchar(mode & S_IXUSR ? 'x' : '-');
    
    // Group permissions
    putchar(mode & S_IRGRP ? 'r' : '-');
    putchar(mode & S_IWGRP ? 'w' : '-');
    putchar(mode & S_IXGRP ? 'x' : '-');
    
    // Other permissions
    putchar(mode & S_IROTH ? 'r' : '-');
    putchar(mode & S_IWOTH ? 'w' : '-');
    putchar(mode & S_IXOTH ? 'x' : '-');
}

// Helper function to print 'ls -l' for a single file
void print_long_listing(const char *filename, const struct stat *s) {
    // 1. Permissions
    print_permissions(s->st_mode);

    // 2. Link count
    printf(" %2ld", s->st_nlink);

    // 3. User name
    struct passwd *pw = getpwuid(s->st_uid);
    if (pw) {
        printf(" %-8s", pw->pw_name);
    } else {
        printf(" %-8d", s->st_uid);
    }

    // 4. Group name
    struct grp *gr = getgrgid(s->st_gid);
    if (gr) {
        printf(" %-8s", gr->gr_name);
    } else {
        printf(" %-8d", s->st_gid);
    }

    // 5. File size
    printf(" %8lld", (long long)s->st_size);

    // 6. Modification time
    char time_buf[64];
    // Format the time: e.g., "Nov 11 09:30"
    strftime(time_buf, sizeof(time_buf), "%b %d %H:%M", localtime(&s->st_mtime));
    printf(" %s", time_buf);

    // 7. File name
    printf(" %s", filename);

    // 8. If it's a symlink, show what it points to
    if (S_ISLNK(s->st_mode)) {
        char link_target[1024];
        ssize_t len = readlink(filename, link_target, sizeof(link_target) - 1);
        if (len != -1) {
            link_target[len] = '\0';
            printf(" -> %s", link_target);
        }
    }
    
    printf("\n");
}

/*
 * Implements 'ls' with support for -a and -l flags.
 */
int ls_main(int argc, char *argv[]) {
    // Flags for options
    int show_all = 0;   // Corresponds to -a
    int long_format = 0; // Corresponds to -l

    int opt;
    // Use getopt() to parse flags
    // The string "al" means we look for -a and -l
    // `optind` is a global variable from unistd.h that tracks
    // the current argument index.
    while ((opt = getopt(argc, argv, "al")) != -1) {
        switch (opt) {
            case 'a':
                show_all = 1;
                break;
            case 'l':
                long_format = 1;
                break;
            case '?': // getopt() prints an error for unknown options
                return 1;
        }
    }
    
    // After getopt, 'optind' points to the first non-option argument
    // A real 'ls' would treat these as paths, but we'll still
    // just list the current directory for simplicity.
    const char *path = ".";

    DIR *d;
    struct dirent *dir;
    
    d = opendir(path);
    if (!d) {
        perror("ls: cannot open directory");
        return 1;
    }

    while ((dir = readdir(d)) != NULL) {
        const char *filename = dir->d_name;

        // Skip hidden files unless -a is specified
        if (!show_all && filename[0] == '.') {
            continue;
        }

        if (long_format) {
            struct stat stat_buf;
            
            // We use lstat() instead of stat() so that if 'filename'
            // is a symlink, we get info about the *link itself*,
            // not the file it points to.
            if (lstat(filename, &stat_buf) == -1) {
                perror("ls: lstat error");
                continue; // Skip this file
            }
            print_long_listing(filename, &stat_buf);
        } else {
            // Simple format
            printf("%s\n", filename);
        }
    }
    closedir(d);
    
    // Reset getopt's global state for the next applet call
    optind = 1; 

    return 0;
}


/*
 * ==================================================================
 * Applet Implementation: editor (Skeleton)
 * (No changes from before)
 * ==================================================================
 */

struct termios orig_termios; // Global for editor exit handler

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
        die("tcsetattr");
    }
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editor_main(int argc, char *argv[]) {
    enableRawMode();
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    char *msg = "smallclu-editor -- Press 'q' to quit.\r\n";
    write(STDOUT_FILENO, msg, strlen(msg));

    while (1) {
        char c = '\0';
        if (read(STDIN_FILENO, &c, 1) == -1) die("read");

        if (c == 'q') {
            break; 
        } else if (iscntrl(c)) {
            char buf[32];
            sprintf(buf, "(%d)\r\n", c);
            write(STDOUT_FILENO, buf, strlen(buf));
        } else {
            char buf[8];
            sprintf(buf, "%c\r\n", c);
            write(STDOUT_FILENO, buf, strlen(buf));
        }
    }
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    return 0;
}


/*
 * ==================================================================
 * Main Dispatcher
 * (No changes from before)
 * ==================================================================
 */
int main(int argc, char *argv[]) {
    
    char *call_name = basename(argv[0]);

    if (strcmp(call_name, "smallclu") == 0) {
        if (argc < 2) {
            fprintf(stderr, "Usage: ./smallclu <applet> [arguments]...\n\n");
            call_name = ""; 
        } else {
            call_name = argv[1];
            argv++;
            argc--;
        }
    }

    if (strcmp(call_name, "echo") == 0) {
        return echo_main(argc, argv);
    } 
    
    if (strcmp(call_name, "ls") == 0) {
        return ls_main(argc, argv);
    }
    
    if (strcmp(call_name, "editor") == 0) {
        return editor_main(argc, argv);
    }
    
    if (strlen(call_name) > 0) {
        fprintf(stderr, "Applet '%s' not found.\n\n", call_name);
    }
    
    fprintf(stderr, "This is 'smallclu'. You can call applets in two ways:\n\n");
    fprintf(stderr, "1. Via symlinks (e.g., 'ln -s smallclu ls'):\n");
    fprintf(stderr, "   ./ls -l\n\n");
    fprintf(stderr, "2. As an argument to smallclu:\n");
    fprintf(stderr, "   ./smallclu ls -l\n\n");
    fprintf(stderr, "Available applets: echo, ls, editor\n");
    
    return 1;
}
