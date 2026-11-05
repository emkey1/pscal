/* The app's own passwd and group tables are read by every thread, and replacing either file reloads
 * it while others read. The load had no lock, so two loading at once could free one table twice and a
 * reader could be left in freed memory. An answer has to stay good for its caller across a reload too:
 * callers don't free it, and until the next call on their own thread is all POSIX asks of them. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) { (void)message; }
#endif

enum { READERS = 6, FIRST_ID = 2000, IDS = 40, REPLACEMENTS = 3000 };

static char g_dir[PATH_MAX];
static atomic_bool g_stop;
static atomic_int g_bad;

/* A file each time: a different size and inode, so that every replacement is a reload. */
static void write_file(const char *leaf, int round, bool group) {
    char tmp[PATH_MAX + 16], path[PATH_MAX + 16];
    snprintf(tmp, sizeof(tmp), "%s/%s.t", g_dir, leaf);
    snprintf(path, sizeof(path), "%s/%s", g_dir, leaf);
    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        perror("fopen");
        exit(2);
    }
    for (int id = FIRST_ID; id < FIRST_ID + IDS + (round % 7); id++) {
        if (group) {
            fprintf(fp, "g%d:x:%d:\n", id, id);
        } else {
            fprintf(fp, "u%d:x:%d:%d:gecos:/home/u%d:/bin/sh\n", id, id, id, id);
        }
    }
    fclose(fp);
    if (rename(tmp, path) != 0) {
        perror("rename");
        exit(2);
    }
}

static void check_name(const char *what, const char *name, int id, char prefix) {
    char want[32];
    snprintf(want, sizeof(want), "%c%d", prefix, id);
    if (!name || strcmp(name, want) != 0) {
        fprintf(stderr, "FAIL: %s %d gave '%s'\n", what, id, name ? name : "(null)");
        atomic_fetch_add(&g_bad, 1);
    }
}

static void *reader(void *arg) {
    int id = FIRST_ID + (int)(intptr_t)arg % IDS;
    char name[32];
    snprintf(name, sizeof(name), "u%d", id);
    char gname[32];
    snprintf(gname, sizeof(gname), "g%d", id);
    while (!atomic_load(&g_stop)) {
        struct passwd *pw = getpwuid((uid_t)id);
        if (pw) {
            check_name("getpwuid", pw->pw_name, id, 'u');
        }
        struct group *gr = getgrgid((gid_t)id);
        if (gr) {
            check_name("getgrgid", gr->gr_name, id, 'g');
        }
        pw = getpwnam(name);
        if (pw && pw->pw_uid != (uid_t)id) {
            fprintf(stderr, "FAIL: getpwnam %s gave uid %d\n", name, (int)pw->pw_uid);
            atomic_fetch_add(&g_bad, 1);
        }
        gr = getgrnam(gname);
        if (gr && gr->gr_gid != (gid_t)id) {
            fprintf(stderr, "FAIL: getgrnam %s gave gid %d\n", gname, (int)gr->gr_gid);
            atomic_fetch_add(&g_bad, 1);
        }
    }
    return NULL;
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof(g_dir), "%s/user-db-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 2;
    }
    setenv("PSCALI_ETC_ROOT", g_dir, 1);
    write_file("passwd", 0, false);
    write_file("group", 0, true);

    /* An answer taken before any of the reloads. */
    struct passwd *kept = getpwuid(FIRST_ID);
    if (!kept) {
        fprintf(stderr, "FAIL: getpwuid %d found nothing in %s\n", FIRST_ID, g_dir);
        return 1;
    }

    pthread_t threads[READERS];
    for (int i = 0; i < READERS; i++) {
        if (pthread_create(&threads[i], NULL, reader, (void *)(intptr_t)i) != 0) {
            perror("pthread_create");
            return 2;
        }
    }
    for (int round = 1; round <= REPLACEMENTS; round++) {
        write_file((round & 1) ? "passwd" : "group", round, (round & 1) == 0);
    }
    atomic_store(&g_stop, true);
    for (int i = 0; i < READERS; i++) {
        pthread_join(threads[i], NULL);
    }

    if (strcmp(kept->pw_name, "u2000") != 0 || kept->pw_uid != FIRST_ID) {
        fprintf(stderr, "FAIL: an answer did not outlive the reloads\n");
        atomic_fetch_add(&g_bad, 1);
    }
    /* The last replacement is what a new question sees. */
    write_file("passwd", 1, false);
    if (!getpwuid(FIRST_ID + IDS)) {
        fprintf(stderr, "FAIL: a new entry in a replaced file was not found\n");
        atomic_fetch_add(&g_bad, 1);
    }

    char path[PATH_MAX + 16];
    snprintf(path, sizeof(path), "%s/passwd", g_dir);
    unlink(path);
    snprintf(path, sizeof(path), "%s/group", g_dir);
    unlink(path);
    rmdir(g_dir);

    int bad = atomic_load(&g_bad);
    if (bad == 0) {
        printf("user db: ok\n");
    }
    return bad ? 1 : 0;
}
