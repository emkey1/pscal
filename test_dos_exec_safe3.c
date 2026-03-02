#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wordexp.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

int safe_exec(const char* path, const char* cmdline) {
    size_t len = strlen(path) + strlen(cmdline) + 2;
    char* cmd = malloc(len);
    if (!cmd) return -1;
    snprintf(cmd, len, "%s %s", path, cmdline);

    int result = -1;
    wordexp_t p;
    // WRDE_NOCMD means "error on command substitution"
    int we_result = wordexp(cmd, &p, WRDE_NOCMD);
    if (we_result == 0) {
        if (p.we_wordc > 0) {
            pid_t pid = fork();
            if (pid == 0) {
                // Child process
                execvp(p.we_wordv[0], p.we_wordv);
                exit(127); // Exit if execvp fails
            } else if (pid > 0) {
                // Parent process
                int status;
                if (waitpid(pid, &status, 0) == pid) {
                    if (WIFEXITED(status)) {
                        result = WEXITSTATUS(status);
                    } else {
                        result = -1;
                    }
                }
            }
        } else {
            result = 0; // Empty command
        }
        wordfree(&p);
    } else {
        printf("wordexp error: %d\n", we_result);
    }
    free(cmd);
    return result;
}

int main() {
    printf("Result 1: %d\n", safe_exec("echo", "hello world"));
    printf("Result 2: %d\n", safe_exec("echo", "hello; cat /etc/passwd"));
    printf("Result 3: %d\n", safe_exec("echo", "hello $(cat /etc/passwd)"));
    return 0;
}
