/* Tabs open and close on threads of one process, and a record lock is the process's, so it keeps
 * none of them out. Without utmp_rec.c's own lock two writers find the same free slot and one login
 * is lost. A session's thread and its tab's removal both end a login, so the second call must do
 * nothing, or it could end the next login on that line. */
#define _GNU_SOURCE 1
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "utmp_rec.h"

enum { WRITERS = 8, EACH = 25 };

static char g_path[PATH_MAX];

static void *writer(void *arg) {
    int w = (int)(intptr_t)arg;
    for (int i = 0; i < EACH; i++) {
        SmallclueUtmp rec;
        memset(&rec, 0, sizeof(rec));
        rec.type = SMALLCLUE_UT_USER_PROCESS;
        rec.pid = w * 100 + i + 1;
        snprintf(rec.line, sizeof(rec.line), "pts/%d", rec.pid);
        snprintf(rec.id, sizeof(rec.id), "%c%03d", 'a' + w, i);
        snprintf(rec.user, sizeof(rec.user), "u");
        if (smallclueUtmpPut(g_path, &rec) != 0) {
            perror("smallclueUtmpPut");
        }
    }
    return NULL;
}

int main(void) {
    const char *dir = getenv("TMPDIR");
    snprintf(g_path, sizeof(g_path), "%s/utmp-writers-%d", dir && dir[0] ? dir : "/tmp", (int)getpid());
    unlink(g_path);

    pthread_t threads[WRITERS];
    for (int w = 0; w < WRITERS; w++) {
        if (pthread_create(&threads[w], NULL, writer, (void *)(intptr_t)w) != 0) {
            perror("pthread_create");
            return 1;
        }
    }
    for (int w = 0; w < WRITERS; w++) {
        pthread_join(threads[w], NULL);
    }

    SmallclueUtmp *recs = NULL;
    int count = smallclueUtmpReadAll(g_path, &recs);
    free(recs);
    unlink(g_path);
    int fails = 0;
    if (count != WRITERS * EACH) {
        fprintf(stderr, "FAIL: %d of %d logins in the file\n", count, WRITERS * EACH);
        fails++;
    }

    /* A session with no login: nothing happens, and in particular no file is touched. */
    smallclueUtmpTerminalClosed(12345);

    if (fails == 0) {
        printf("utmp writers: ok\n");
    }
    return fails ? 1 : 0;
}
