/* Keys typed at a session's terminal, read the way a command in the app reads
 * them: through the shims, as a pipeline stage. Each case runs in its own process,
 * so that a lost key fails that case by timing out instead of ending the run. */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#include "ios/tty/pscal_pty.h"
#include "ios/tty/pscal_tty_host.h"

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif

#include "ios/vproc.h"

#define KEY_WAIT_MS 2000
#define NOTHING_WAIT_MS 150

typedef struct {
    pthread_mutex_t mu;
    char buf[256];
    size_t len;
} echo_capture;

typedef struct {
    uint64_t id;
    int pty_num;
    struct pscal_fd *master;
    struct pscal_fd *slave;
    VProcSessionStdio *session;
    VProc *vp;
    echo_capture echoed;
} terminal;

static void echo_handler(uint64_t session_id, const unsigned char *data, size_t len, void *context) {
    (void)session_id;
    echo_capture *capture = (echo_capture *)context;
    pthread_mutex_lock(&capture->mu);
    size_t room = sizeof(capture->buf) - 1 - capture->len;
    size_t n = len < room ? len : room;
    memcpy(capture->buf + capture->len, data, n);
    capture->len += n;
    capture->buf[capture->len] = '\0';
    pthread_mutex_unlock(&capture->mu);
}

static void terminal_set_lflags(terminal *t, dword_t set, dword_t clear) {
    struct termios_ termios;
    assert(t->slave->ops->ioctl(t->slave, TCGETS_, &termios) == 0);
    termios.lflags = (termios.lflags | set) & ~clear;
    assert(t->slave->ops->ioctl(t->slave, TCSETS_, &termios) == 0);
}

static void terminal_set_raw(terminal *t) {
    terminal_set_lflags(t, 0, ICANON_ | ECHO_ | ISIG_);
}

/* What the shell gives a command: line mode, echo, and ISIG, as on Linux (L20). */
static void terminal_set_line(terminal *t) {
    terminal_set_lflags(t, ICANON_ | ECHO_ | ISIG_, 0);
}

/* shell_pid, if there is one, is who the session's reader takes itself for. */
static void terminal_open_for_shell(terminal *t, int shell_pid) {
    static uint64_t next_id = 4100;
    memset(t, 0, sizeof(*t));
    assert(pscalPtyOpenMaster(O_RDWR, &t->master, &t->pty_num) == 0);
    assert(pscalPtyUnlock(t->master) == 0);
    assert(pscalPtyOpenSlave(t->pty_num, O_RDWR, &t->slave) == 0);
    t->id = next_id++;
    t->session = vprocSessionStdioCreate();
    assert(t->session);
    assert(vprocSessionStdioInitWithPty(t->session, t->slave, t->master, t->id, 0) == 0);
    pthread_mutex_init(&t->echoed.mu, NULL);
    vprocSessionSetOutputHandler(t->id, echo_handler, &t->echoed);
    vprocSessionStdioActivate(t->session);
    t->vp = vprocCreate(NULL);
    assert(t->vp);
    vprocRegisterThread(t->vp, pthread_self());
    vprocActivate(t->vp);
    /* A tab's shell leads a session of its own, and claims its terminal once it has a task:
     * that gives the terminal to its session with its group in the foreground, where the
     * terminal sends Ctrl-C (L20). */
    (void)vprocSetSid(vprocPid(t->vp), vprocPid(t->vp));
    vprocSessionStdioActivate(t->session);
    /* Raw, as the shell's prompt leaves it. A read goes to the terminal itself (L20): there
     * is no reader thread. */
    terminal_set_raw(t);
    t->session->shell_pid = shell_pid;
    vprocSetPipelineStage(true);
}

static void terminal_open(terminal *t) {
    terminal_open_for_shell(t, 0);
}

static void type_keys(terminal *t, const void *keys, size_t len) {
    assert(vprocSessionWriteToMaster(t->id, keys, len) == (ssize_t)len);
}

static void type_str(terminal *t, const char *keys) {
    type_keys(t, keys, strlen(keys));
}

/* A command's read of fd: bytes, 0 for end of file, -1 with errno, or -2 when
 * nothing came in time. */
static ssize_t read_fd(int fd, void *buf, size_t count, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
    int ready = vprocPollShim(&pfd, 1, timeout_ms);
    if (ready == 0) {
        return -2;
    }
    /* A signal ends a poll as it does a read: the read says what it did to the input. */
    assert(ready > 0 || errno == EINTR);
    return vprocReadShim(fd, buf, count);
}

static ssize_t read_terminal(void *buf, size_t count, int timeout_ms) {
    return read_fd(STDIN_FILENO, buf, count, timeout_ms);
}

static void expect_text(const char *want) {
    size_t want_len = strlen(want);
    char got[512];
    size_t len = 0;
    while (len < want_len) {
        ssize_t n = read_terminal(got + len, want_len - len, KEY_WAIT_MS);
        if (n <= 0) {
            fprintf(stderr, "wanted [%s], got [%.*s] then read %zd\n", want, (int)len, got, n);
        }
        assert(n > 0);
        len += (size_t)n;
    }
    assert(memcmp(got, want, want_len) == 0);
}

static void expect_nothing(void) {
    char c;
    assert(read_terminal(&c, 1, NOTHING_WAIT_MS) == -2);
}

static void expect_eof(void) {
    char c;
    assert(read_terminal(&c, 1, KEY_WAIT_MS) == 0);
}

static void expect_echo(terminal *t, const char *want) {
    for (int waited = 0; waited < KEY_WAIT_MS; waited += 10) {
        pthread_mutex_lock(&t->echoed.mu);
        bool seen = strstr(t->echoed.buf, want) != NULL;
        pthread_mutex_unlock(&t->echoed.mu);
        if (seen) {
            return;
        }
        usleep(10000);
    }
    fprintf(stderr, "wanted echo [%s], have [%s]\n", want, t->echoed.buf);
    assert(!"echo");
}

/* --- raw mode: no key goes astray --- */

static void case_raw_every_key_one_at_a_time(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_raw(&t);
    for (int i = 0; i < 300; i++) {
        char key = (char)('a' + i % 26);
        char got = 0;
        type_keys(&t, &key, 1);
        assert(read_terminal(&got, 1, KEY_WAIT_MS) == 1);
        assert(got == key);
    }
}

static void case_raw_burst_stays_in_order(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_raw(&t);
    char keys[1000];
    for (size_t i = 0; i < sizeof(keys); i++) {
        keys[i] = (char)('a' + i % 26);
    }
    type_keys(&t, keys, sizeof(keys));
    char got[sizeof(keys)];
    size_t len = 0;
    while (len < sizeof(got)) {
        ssize_t n = read_terminal(got + len, sizeof(got) - len, KEY_WAIT_MS);
        assert(n > 0);
        len += (size_t)n;
    }
    assert(memcmp(got, keys, sizeof(keys)) == 0);
}

/* Keys taken by a read through another open of the terminal are the same keys. */
static void case_second_open_of_the_terminal(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_raw(&t);
    struct pscal_fd *other = NULL;
    assert(pscalPtyOpenSlave(t.pty_num, O_RDWR, &other) == 0);
    int fd = 10;
    assert(vprocAdoptPscalFd(t.vp, fd, other) == 0);
    for (int i = 0; i < 100; i++) {
        char key = (char)('a' + i % 26);
        char got = 0;
        type_keys(&t, &key, 1);
        assert(read_fd(fd, &got, 1, KEY_WAIT_MS) == 1);
        assert(got == key);
    }
}

/* --- line mode: what a command gets by default --- */

static void case_line_waits_for_return_and_echoes(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    type_str(&t, "ab");
    expect_nothing();
    type_str(&t, "c\r");
    expect_text("abc\n");
    expect_echo(&t, "abc\r\n");
}

static void case_line_every_key_of_many_lines(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    for (int i = 0; i < 100; i++) {
        char line[32];
        snprintf(line, sizeof(line), "the quick brown %d\n", i);
        for (const char *key = line; *key; key++) {
            type_keys(&t, key, 1);
        }
        expect_text(line);
    }
}

/* A terminal hands over one line per read, so what is typed ahead of the next
 * line stays for whoever reads next. */
static void case_line_one_line_per_read(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    type_str(&t, "one\ntwo\n");
    usleep(50000);
    char got[64];
    assert(read_terminal(got, sizeof(got), KEY_WAIT_MS) == 4);
    assert(memcmp(got, "one\n", 4) == 0);
    assert(read_terminal(got, sizeof(got), KEY_WAIT_MS) == 4);
    assert(memcmp(got, "two\n", 4) == 0);
}

static void case_line_erase_key(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    type_str(&t, "ax\x7f" "b\n");
    expect_text("ab\n");
}

static void case_line_end_of_file_key(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    /* Alone on a line it is the end of the file, for one read, and input goes on. */
    type_str(&t, "\x04");
    expect_eof();
    type_str(&t, "next\n");
    expect_text("next\n");
    /* After part of a line it hands over that part. */
    type_str(&t, "xy\x04");
    expect_text("xy");
    expect_nothing();
    type_str(&t, "\x04");
    expect_eof();
    /* Input ahead of it goes first. */
    type_str(&t, "one\n\x04two\n");
    usleep(50000);
    expect_text("one\n");
    expect_eof();
    expect_text("two\n");
}

/* A key typed after its command ended must not end the session at the prompt. */
static void case_prompt_ignores_old_end_of_file_key(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    /* Ctrl-D typed while a command ran and nothing read it. The prompt's line editor reads in
     * raw mode, where that end-of-file key is a byte 0, as on Linux: not the end of the shell's
     * input, so the tab stays open. */
    type_str(&t, "\x04");
    usleep(50000);
    terminal_set_raw(&t);
    char c = 1;
    assert(read_terminal(&c, 1, KEY_WAIT_MS) == 1);
    assert(c == 0);
    terminal_set_line(&t);
    type_str(&t, "\x04");
    expect_eof();
}

/* Longer than the terminal's buffer, with no end in sight: nothing can read it,
 * and writing it must not wait for a reader. An alarm ends this if it does. */
static void case_line_too_long_for_the_buffer(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    static char keys[TTY_BUF_SIZE + 1000];
    memset(keys, 'x', sizeof(keys));
    type_keys(&t, keys, sizeof(keys));
    type_str(&t, "\n");
    static char got[TTY_BUF_SIZE];
    size_t len = 0;
    while (len < TTY_BUF_SIZE) {
        ssize_t n = read_terminal(got + len, TTY_BUF_SIZE - len, KEY_WAIT_MS);
        assert(n > 0);
        len += (size_t)n;
    }
    assert(got[TTY_BUF_SIZE - 1] == '\n');
    expect_nothing();
}

/* --- Ctrl-C and Ctrl-Z in line mode --- */

static void terminal_run_command(terminal *t);
static void terminal_end_command(void);

static void expect_control_key_or_interrupt(unsigned char key) {
    unsigned char got = 0;
    ssize_t n = read_terminal(&got, 1, KEY_WAIT_MS);
    assert(n >= -1 && n <= 1);
    if (n == 1) {
        assert(got == key);
    } else if (n == 0) {
        assert(key == 0x03 && vprocTakeTerminalInterrupted());
    } else {
        assert(errno == EINTR);
    }
}

static void case_line_control_keys_get_past_the_line(void) {
    terminal t;
    terminal_open(&t);
    terminal_run_command(&t);
    /* No Return follows: the key must not wait for one, and what was typed of
     * the line is gone with it (isig, L20). */
    type_str(&t, "ab\x03");
    expect_control_key_or_interrupt(0x03);
    terminal_end_command();
    terminal_run_command(&t);
    type_str(&t, "next\n");
    expect_text("next\n");
}

/* The same bytes with no session reader stay data, as on any terminal. */
static void case_control_keys_stay_data_without_session_reader(void) {
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(num, O_RDWR, &slave) == 0);
    struct termios_ termios;
    assert(slave->ops->ioctl(slave, TCGETS_, &termios) == 0);
    termios.lflags = (termios.lflags | ICANON_) & ~(ISIG_ | ECHO_);
    assert(slave->ops->ioctl(slave, TCSETS_, &termios) == 0);
    assert(tty_input(slave->tty, "ab\x03" "c\n", 5, true) == 5);
    char got[8] = {0};
    assert(slave->ops->read(slave, got, sizeof(got)) == 5);
    assert(memcmp(got, "ab\x03" "c\n", 5) == 0);
}

/* --- the interrupt key at a command waiting for a line --- */

/* A command as the app runs it. exsh_main blocks SIGINT, so on every vproc the
 * signal only waits; nothing in it can wake or end a read. */
/* The command terminal_run_command started, if it is still running. */
static VProc *gCommand = NULL;

static void terminal_run_command(terminal *t) {
    terminal_set_line(t);
    /* t->vp is the shell: it blocks SIGINT for its own catcher, as exsh does. */
    int shell = vprocPid(t->vp);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    assert(vprocPthreadSigmaskShim(SIG_BLOCK, &set, NULL) == 0);
    vprocSetPgid(shell, shell);
    t->session->shell_pid = shell;
    vprocSetShellSelfPid(shell);
    vprocSetShellSelfTid(pthread_self());
    (void)vprocRegisterTidHint(shell, pthread_self());
    (void)vprocSetShellTask(shell, true);
    /* The command is a task of its own on the shell's thread, in a group of its own in the
     * foreground, as the app runs a pipeline stage. It starts with SIGINT unblocked. */
    VProc *command = vprocCreate(NULL);
    assert(command);
    gCommand = command;
    vprocRegisterThread(command, pthread_self());
    vprocActivate(command);
    int pid = vprocPid(command);
    assert(vprocSetPgid(pid, pid) == 0);
    int sid = vprocGetSid(shell);
    assert(sid > 0);
    assert(vprocSetForegroundPgid(sid, pid) == 0);
}

/* The command ends, as its task would, and the shell is current again. */
static void terminal_end_command(void) {
    if (!gCommand) {
        return;
    }
    const int pid = vprocPid(gCommand);
    vprocDeactivate();
    vprocMarkExit(gCommand, 128 + SIGINT);
    vprocDiscard(pid);
    vprocDestroy(gCommand);
    gCommand = NULL;
    /* The shell takes the terminal back, as exsh does after a command. */
    const int shell = vprocGetShellSelfPid();
    const int sid = shell > 0 ? vprocGetSid(shell) : -1;
    if (sid > 0) {
        (void)vprocSetForegroundPgid(sid, vprocGetPgid(shell));
    }
}

typedef struct {
    terminal *t;
    const char *keys;
} delayed_keys;

static void *type_later(void *arg) {
    delayed_keys *later = (delayed_keys *)arg;
    usleep(100000);
    type_str(later->t, later->keys);
    return NULL;
}

/* A read that is already waiting, the way a read loop would make it. */
static ssize_t read_waiting_for_keys(terminal *t, const char *keys, char *got, size_t count) {
    delayed_keys later = {t, keys};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, type_later, &later) == 0);
    ssize_t n = vprocReadShim(STDIN_FILENO, got, count);
    pthread_join(thread, NULL);
    return n;
}

static void case_interrupt_key_ends_a_waiting_read(void) {
    terminal t;
    terminal_open(&t);
    terminal_run_command(&t);
    type_str(&t, "gone");
    expect_nothing();
    char got[16];
    assert(read_waiting_for_keys(&t, "\x03", got, sizeof(got)) == 0);
    /* The terminal echoes the key, as Linux's does; the new line is the shell's. */
    expect_echo(&t, "gone^C");
    /* A loop that goes on reading stays stopped, whatever it is. */
    assert(vprocReadShim(STDIN_FILENO, got, sizeof(got)) == 0);
    assert(vprocTakeTerminalInterrupted());
    assert(!vprocTakeTerminalInterrupted());
    /* The next command reads again, and what was typed before the key is not there. */
    terminal_end_command();
    terminal_run_command(&t);
    type_str(&t, "next\n");
    expect_text("next\n");
}

/* Lines typed ahead of the key must not run after what it stopped. */
static void case_interrupt_key_takes_lines_typed_ahead(void) {
    terminal t;
    terminal_open(&t);
    terminal_run_command(&t);
    type_str(&t, "one\ntwo\n\x03");
    usleep(50000);
    char got[16];
    assert(vprocReadShim(STDIN_FILENO, got, sizeof(got)) == 0);
    assert(vprocTakeTerminalInterrupted());
    terminal_end_command();
    terminal_run_command(&t);
    expect_nothing();
}

/* Keys that a program in raw mode asked for: with isig clear the key is a byte, as on Linux,
 * and its input stays open. */
static void case_interrupt_key_leaves_raw_mode_input_open(void) {
    terminal t;
    terminal_open(&t);
    terminal_run_command(&t);
    terminal_set_raw(&t);
    char got[16];
    errno = 0;
    assert(read_waiting_for_keys(&t, "\x03", got, sizeof(got)) == 1);
    assert(got[0] == 0x03);
    assert(!vprocTakeTerminalInterrupted());
    type_str(&t, "k");
    expect_text("k");
}

/* Ctrl-C at one tab's terminal reaches that tab's foreground job and no other tab's. */
static void case_interrupt_key_stays_in_its_tab(void) {
    terminal a;
    terminal_open(&a);
    terminal_run_command(&a);
    const int a_pid = vprocPid(gCommand);

    /* The other tab: a terminal of its own, whose session's foreground job holds SIGINT
     * blocked, so that one sent to it stays to be seen. */
    struct pscal_fd *b_master = NULL;
    struct pscal_fd *b_slave = NULL;
    int b_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &b_master, &b_num) == 0);
    assert(pscalPtyUnlock(b_master) == 0);
    assert(pscalPtyOpenSlave(b_num, O_RDWR, &b_slave) == 0);
    VProc *b_job = vprocCreate(NULL);
    assert(b_job);
    const int b_pid = vprocPid(b_job);
    assert(vprocSetSid(b_pid, b_pid) == 0);
    b_slave->tty->session = (pid_t_)b_pid;
    b_slave->tty->fg_group = (pid_t_)b_pid;
    vprocActivate(b_job);
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGINT);
    assert(vprocPthreadSigmaskShim(SIG_BLOCK, &block, NULL) == 0);
    vprocDeactivate();

    sigset_t pending;
    /* The key at the other tab: its job has it, this tab's command does not. */
    assert(b_master->ops->write(b_master, "\x03", 1) == 1);
    usleep(50000);
    sigemptyset(&pending);
    assert(vprocSigpending(b_pid, &pending) == 0 && sigismember(&pending, SIGINT));
    sigemptyset(&pending);
    assert(vprocSigpending(a_pid, &pending) == 0 && !sigismember(&pending, SIGINT));
    char got[16];
    assert(read_terminal(got, sizeof(got), NOTHING_WAIT_MS) == -2);
    /* The key here: this tab's command's input ends. */
    assert(read_waiting_for_keys(&a, "\x03", got, sizeof(got)) == 0);
    assert(vprocTakeTerminalInterrupted());

    vprocMarkExit(b_job, 0);
    vprocDiscard(b_pid);
    vprocDestroy(b_job);
    pscal_fd_close(b_slave);
    pscal_fd_close(b_master);
}

/* At the prompt it is a key: the line editor throws away the line being edited. */
static void case_prompt_takes_the_interrupt_key(void) {
    terminal t;
    terminal_open(&t);
    terminal_run_command(&t);
    terminal_set_raw(&t);
    int pid = vprocPid(t.vp);
    vprocSetPipelineStage(false);
    vprocSetShellPromptReadActive(pid, true);
    unsigned char got = 0;
    assert(read_waiting_for_keys(&t, "\x03", (char *)&got, 1) == 1);
    assert(got == 0x03);
    assert(!vprocTakeTerminalInterrupted());
    /* No signal is left waiting for something else to take it. */
    sigset_t pending;
    sigemptyset(&pending);
    assert(vprocSigpending(pid, &pending) == 0);
    assert(!sigismember(&pending, SIGINT));
    type_str(&t, "k");
    expect_text("k");
}

/* --- a read already waiting follows a change of mode --- */

static void case_waiting_reader_follows_change_to_raw(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    /* The reader has gone back to its read with the terminal in line mode. */
    type_str(&t, "one\n");
    expect_text("one\n");
    usleep(50000);
    terminal_set_raw(&t);
    usleep(50000);
    type_str(&t, "k");
    expect_text("k");
}

static void case_waiting_reader_follows_change_to_line(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_raw(&t);
    type_str(&t, "r");
    expect_text("r");
    usleep(50000);
    terminal_set_line(&t);
    usleep(50000);
    /* Not a byte 0 from a read still waiting as if raw. */
    type_str(&t, "\x04");
    expect_eof();
    type_str(&t, "ab");
    expect_nothing();
    type_str(&t, "\n");
    expect_text("ab\n");
}

/* The way the app switches at every command: raw at the prompt, line mode for the
 * command, and back, with keys in between. */
static void case_prompt_and_command_turns(void) {
    terminal t;
    terminal_open(&t);
    for (int i = 0; i < 50; i++) {
        terminal_set_raw(&t);
        type_str(&t, "p");
        expect_text("p");
        terminal_set_line(&t);
        type_str(&t, "cmd\n");
        expect_text("cmd\n");
        type_str(&t, "\x04");
        expect_eof();
    }
}

/* --- poll --- */

static void case_poll_sees_lines_and_the_end_of_file_key(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    struct pollfd pfd = {.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
    assert(vprocPollShim(&pfd, 1, NOTHING_WAIT_MS) == 0);
    type_str(&t, "ab");
    pfd.revents = 0;
    assert(vprocPollShim(&pfd, 1, NOTHING_WAIT_MS) == 0);
    type_str(&t, "\n");
    pfd.revents = 0;
    assert(vprocPollShim(&pfd, 1, KEY_WAIT_MS) == 1);
    assert(pfd.revents & POLLIN);
    expect_text("ab\n");
    type_str(&t, "\x04");
    pfd.revents = 0;
    assert(vprocPollShim(&pfd, 1, KEY_WAIT_MS) == 1);
    assert(pfd.revents & POLLIN);
    expect_eof();
}

/* --- the terminal's modes in Linux's numbers, as stty asks for them --- */

static void case_termios_in_linux_numbers(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    struct {
        struct termios_ termios;
        unsigned char after[64];
    } k;
    memset(&k, 0x5a, sizeof(k));
    assert(vprocIoctlShim(STDIN_FILENO, TCGETS_, &k) == 0);
    for (size_t i = 0; i < sizeof(k.after); i++) {
        assert(k.after[i] == 0x5a);
    }
    assert(k.termios.lflags & ICANON_);
    assert(k.termios.lflags & ECHO_);
    assert(k.termios.cc[VEOF_] == 4);
    k.termios.lflags &= ~ECHO_;
    assert(vprocIoctlShim(STDIN_FILENO, TCSETSW_, &k) == 0);
    struct termios_ now;
    assert(t.slave->ops->ioctl(t.slave, TCGETS_, &now) == 0);
    assert(!(now.lflags & ECHO_));
    assert(now.lflags & ICANON_);
    assert(now.cc[VEOF_] == 4);
    /* It keeps working as a terminal, with no echo. */
    type_str(&t, "quiet\n");
    expect_text("quiet\n");
    expect_nothing();
    pthread_mutex_lock(&t.echoed.mu);
    assert(strstr(t.echoed.buf, "quiet") == NULL);
    pthread_mutex_unlock(&t.echoed.mu);
}

/* The same in the host's numbers and the host's struct, as the shell's read asks. */
static void case_termios_in_the_hosts_numbers(void) {
    terminal t;
    terminal_open(&t);
    terminal_set_line(&t);
    struct termios host;
    assert(vprocIoctlShim(STDIN_FILENO, TIOCGETA, &host) == 0);
    assert(host.c_lflag & ICANON);
    assert(host.c_lflag & ECHO);
    struct termios keys = host;
    keys.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    keys.c_cc[VMIN] = 1;
    keys.c_cc[VTIME] = 0;
    /* TIOCSETAW and its kin have the top bit set. */
    unsigned long request = TIOCSETAW;
    assert(vprocIoctlShim(STDIN_FILENO, request, &keys) == 0);
    type_str(&t, "k");
    expect_text("k");
    pthread_mutex_lock(&t.echoed.mu);
    assert(strchr(t.echoed.buf, 'k') == NULL);
    pthread_mutex_unlock(&t.echoed.mu);
    request = TIOCSETAW;
    assert(vprocIoctlShim(STDIN_FILENO, request, &host) == 0);
    type_str(&t, "line");
    expect_nothing();
    type_str(&t, "\n");
    expect_text("line\n");
}

/* --- /dev/tty: each tab's commands reach that tab's own terminal --- */

static bool echo_has(terminal *t, const char *want, int wait_ms) {
    for (int waited = 0; waited <= wait_ms; waited += 10) {
        pthread_mutex_lock(&t->echoed.mu);
        bool seen = strstr(t->echoed.buf, want) != NULL;
        pthread_mutex_unlock(&t->echoed.mu);
        if (seen) {
            return true;
        }
        usleep(10000);
    }
    return false;
}

/* Open /dev/tty as a command does and write a line to it. */
static void write_dev_tty(const char *text) {
    int fd = vprocOpenShim("/dev/tty", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "/dev/tty open failed errno=%d\n", errno);
    }
    assert(fd >= 0);
    assert(vprocWriteShim(fd, text, strlen(text)) == (ssize_t)strlen(text));
    assert(vprocCloseShim(fd) == 0);
}

static void case_dev_tty_is_the_tabs_terminal(void) {
    terminal t;
    terminal_open(&t);
    terminal_run_command(&t);
    write_dev_tty("to-tty\n");
    assert(echo_has(&t, "to-tty", KEY_WAIT_MS));
}

/* `read x < /dev/tty`, `head -1 < /dev/tty`: typed lines come through it, one per read,
 * and the next read through the command's own stdin gets the line after: both read the
 * session's queue. */
static void case_dev_tty_reads_typed_lines(void) {
    terminal t;
    terminal_open(&t);
    terminal_run_command(&t);
    int fd = vprocOpenShim("/dev/tty", O_RDONLY);
    assert(fd >= 0);
    char got[64];
    type_str(&t, "one\ntwo\n");
    usleep(50000);
    ssize_t n = read_fd(fd, got, sizeof(got), KEY_WAIT_MS);
    assert(n == 4 && memcmp(got, "one\n", 4) == 0);
    assert(vprocCloseShim(fd) == 0);
    expect_text("two\n");
}

/* halt from a job, or kill -HUP from another tab: the shell waiting at its prompt takes it
 * then, not when a line is next typed. Such a signal waits with a shell's task until it looks. */
typedef struct {
    int pid;
    terminal *t;
} prompt_kill;

static void *kill_then_type(void *arg) {
    prompt_kill *pk = (prompt_kill *)arg;
    usleep(200000);
    assert(vprocKillShim(pk->pid, SIGHUP) == 0);
    /* Only for a read that never woke: the test then fails instead of hanging. */
    usleep(1500000);
    type_str(pk->t, "late\n");
    return NULL;
}

static void case_prompt_read_wakes_for_a_signal_that_ends_the_shell(void) {
    terminal t;
    terminal_open(&t);
    int pid = vprocPid(t.vp);
    vprocSetUnwindsItself(pid, true);
    vprocSetShellPromptReadActive(pid, true);
    vprocSetPipelineStage(false);
    prompt_kill pk = {pid, &t};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, kill_then_type, &pk) == 0);
    char got[16];
    ssize_t n = vprocReadShim(STDIN_FILENO, got, sizeof(got));
    int err = errno;
    pthread_join(thread, NULL);
    if (n >= 0) {
        fprintf(stderr, "prompt read gave %zd bytes, not EINTR\n", n);
    }
    assert(n < 0 && err == EINTR);
}

/* A second tab as the app opens one: its thread takes the session before its
 * shell has a task, when the session id it finds is the first tab's shell. */
typedef struct {
    terminal t;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int step;
    int done;
} second_tab;

static void second_tab_wait(second_tab *tab, int step) {
    pthread_mutex_lock(&tab->mu);
    while (tab->step < step) {
        pthread_cond_wait(&tab->cv, &tab->mu);
    }
    pthread_mutex_unlock(&tab->mu);
}

static void second_tab_done(second_tab *tab, int done) {
    pthread_mutex_lock(&tab->mu);
    tab->done = done;
    pthread_cond_broadcast(&tab->cv);
    pthread_mutex_unlock(&tab->mu);
}

static void second_tab_wait_done(second_tab *tab, int done) {
    pthread_mutex_lock(&tab->mu);
    while (tab->done < done) {
        pthread_cond_wait(&tab->cv, &tab->mu);
    }
    pthread_mutex_unlock(&tab->mu);
}

static void second_tab_go(second_tab *tab, int step) {
    pthread_mutex_lock(&tab->mu);
    tab->step = step;
    pthread_cond_broadcast(&tab->cv);
    pthread_mutex_unlock(&tab->mu);
}

static void *second_tab_thread(void *arg) {
    second_tab *tab = (second_tab *)arg;
    terminal_open(&tab->t);
    int pid = vprocPid(tab->t.vp);
    vprocSetSid(pid, pid);
    vprocSetShellSelfPid(pid);
    terminal_run_command(&tab->t);
    second_tab_done(tab, 1);
    second_tab_wait(tab, 2);
    write_dev_tty("from-second\n");
    second_tab_done(tab, 2);
    second_tab_wait(tab, 3);
    vprocSessionStdioDestroy(tab->t.session);
    second_tab_done(tab, 3);
    return NULL;
}

static void *set_global_shell_pid(void *arg) {
    vprocSetShellSelfPid(*(int *)arg);
    return NULL;
}

static void case_dev_tty_follows_each_tab(void) {
    terminal first;
    terminal_open(&first);
    int first_pid = vprocPid(first.vp);
    vprocSetSid(first_pid, first_pid);
    terminal_run_command(&first);
    /* The first tab's shell is the one a thread with no task of its own finds. */
    pthread_t hint;
    assert(pthread_create(&hint, NULL, set_global_shell_pid, &first_pid) == 0);
    pthread_join(hint, NULL);

    second_tab tab;
    memset(&tab, 0, sizeof(tab));
    pthread_mutex_init(&tab.mu, NULL);
    pthread_cond_init(&tab.cv, NULL);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, second_tab_thread, &tab) == 0);
    second_tab_wait_done(&tab, 1);

    write_dev_tty("from-first\n");
    assert(echo_has(&first, "from-first", KEY_WAIT_MS));
    assert(!echo_has(&tab.t, "from-first", NOTHING_WAIT_MS));

    second_tab_go(&tab, 2);
    second_tab_wait_done(&tab, 2);
    assert(echo_has(&tab.t, "from-second", KEY_WAIT_MS));
    assert(!echo_has(&first, "from-second", NOTHING_WAIT_MS));

    /* The second tab closes; the first tab's /dev/tty is still its terminal. */
    second_tab_go(&tab, 3);
    second_tab_wait_done(&tab, 3);
    pthread_join(thread, NULL);
    write_dev_tty("after-close\n");
    assert(echo_has(&first, "after-close", KEY_WAIT_MS));
}

/* --- job control and the session's reader --- */

/* A session whose shell is one process and whose current process, t.vp, is another, in a
 * group of its own, with the terminal in the group named. */
static void terminal_open_as_job(terminal *t, bool job_in_foreground) {
    VProc *shell = vprocCreate(NULL);
    assert(shell);
    int shell_pid = vprocPid(shell);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    terminal_open_for_shell(t, shell_pid);
    int pid = vprocPid(t->vp);
    assert(vprocSetSid(pid, shell_pid) == 0);
    assert(vprocSetPgid(pid, pid) == 0);
    vprocSetParent(pid, shell_pid);
    int foreground = job_in_foreground ? pid : shell_pid;
    assert(vprocSetForegroundPgid(shell_pid, foreground) == 0);
    struct tty *tty = t->slave->tty;
    pscalTtyClearControlling(tty);
    lock(&tty->lock);
    tty->session = shell_pid;
    tty->fg_group = foreground;
    unlock(&tty->lock);
    pscalTtySetControlling(tty);
}

/* The reader has no process of its own, and goes by the shell's name. With a job in the
 * foreground that makes it look like a background reader of the terminal: it must not
 * stop anything, or be refused, for that. Refused is what it would be with the shell's
 * SIGTTIN ignored, as a shell's is; with that not so, the shell would be stopped. */
static void reader_keeps_reading(bool ignored) {
    terminal t;
    terminal_open_as_job(&t, true);
    int shell_pid = t.session->shell_pid;
    if (ignored) {
        assert(vprocSigaction(shell_pid, SIGTTIN, &(struct sigaction){.sa_handler = SIG_IGN}, NULL) == 0);
    }
    for (int i = 0; i < 20; i++) {
        char key = (char)('a' + i);
        char got = 0;
        type_keys(&t, &key, 1);
        assert(read_terminal(&got, 1, KEY_WAIT_MS) == 1);
        assert(got == key);
    }
    size_t capacity = vprocSnapshot(NULL, 0);
    VProcSnapshot *snapshots = calloc(capacity ? capacity : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snapshots, capacity);
    for (size_t i = 0; i < count; i++) {
        assert(!snapshots[i].stopped);
    }
    free(snapshots);
}

static void case_reader_is_not_a_background_process(void) {
    reader_keeps_reading(false);
}

static void case_reader_is_not_refused_as_one(void) {
    reader_keeps_reading(true);
}

/* A real background read of the session's terminal, through its queue. Blocked or ignored,
 * SIGTTIN can't stop it, and the read is told so rather than handed the foreground's keys. */
static void case_background_read_of_the_session_terminal(void) {
    terminal t;
    terminal_open_as_job(&t, false);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTTIN);
    assert(vprocPthreadSigmaskShim(SIG_BLOCK, &set, NULL) == 0);
    char c;
    errno = 0;
    assert(vprocReadShim(STDIN_FILENO, &c, 1) == -1);
    assert(errno == EIO);
    assert(vprocPthreadSigmaskShim(SIG_UNBLOCK, &set, NULL) == 0);
    assert(vprocSigactionShim(SIGTTIN, &(struct sigaction){.sa_handler = SIG_IGN}, NULL) == 0);
    errno = 0;
    assert(vprocReadShim(STDIN_FILENO, &c, 1) == -1);
    assert(errno == EIO);
}

/* --- the foreground group has one record --- */

typedef struct {
    int sid;
    int pgid;
} foreground_word;

static void *hand_over_foreground(void *arg) {
    const foreground_word *word = (const foreground_word *)arg;
    assert(vprocSetForegroundPgid(word->sid, word->pgid) == 0);
    return NULL;
}

/* From a thread with no session and no process, as the shell's hand-off may be. */
static void hand_over_from_nobody(int sid, int pgid) {
    foreground_word word = {sid, pgid};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, hand_over_foreground, &word) == 0);
    pthread_join(thread, NULL);
}

static volatile sig_atomic_t g_winches = 0;

static void count_winch(int sig) {
    (void)sig;
    g_winches++;
}

static void case_foreground_handoff_reaches_the_terminal(void) {
    terminal t;
    terminal_open_as_job(&t, false);
    int shell_pid = t.session->shell_pid;
    int pid = vprocPid(t.vp);
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = count_winch;
    assert(vprocSigactionShim(SIGWINCH, &action, NULL) == 0);

    /* The job in the foreground. */
    hand_over_from_nobody(shell_pid, pid);
    int value = -1;
    assert(vprocIoctlShim(STDIN_FILENO, TIOCGPGRP_, &value) == 0);
    assert(value == pid);
    assert(vprocTcgetpgrpShim(STDIN_FILENO) == pid);
    /* A resize reaches it. */
    assert(vprocSetSessionWinsize(t.id, 90, 30) == 0);
    assert(g_winches == 1);
    /* And it reads as the foreground does, its keys and no SIGTTIN. */
    type_str(&t, "k");
    expect_text("k");

    /* Back to the shell: the job gets neither, and is a background reader again. */
    hand_over_from_nobody(shell_pid, shell_pid);
    assert(vprocIoctlShim(STDIN_FILENO, TIOCGPGRP_, &value) == 0);
    assert(value == shell_pid);
    assert(vprocSetSessionWinsize(t.id, 91, 31) == 0);
    assert(g_winches == 1);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTTIN);
    assert(vprocPthreadSigmaskShim(SIG_BLOCK, &set, NULL) == 0);
    char c;
    errno = 0;
    assert(vprocReadShim(STDIN_FILENO, &c, 1) == -1);
    assert(errno == EIO);
}

/* A tab's first thread can't say whose session the terminal is, so it leaves that for the
 * shell, whose claim then goes through. As exsh does it. */
static void case_shell_claims_a_terminal_nobody_took(void) {
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(num, O_RDWR, &slave) == 0);
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, 4900, 0) == 0);
    vprocSessionStdioActivate(session);
    assert(slave->tty->session == 0);

    VProc *shell = vprocCreate(NULL);
    assert(shell);
    int shell_pid = vprocPid(shell);
    vprocRegisterThread(shell, pthread_self());
    vprocSetShellSelfPid(shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);
    vprocActivate(shell);
    assert(slave->ops->ioctl(slave, TIOCSCTTY_, NULL) == 0);
    assert(slave->tty->session == shell_pid);
    assert(slave->tty->fg_group == shell_pid);

    /* Now the hand-off has somewhere to go. */
    VProc *job = vprocCreate(NULL);
    assert(job);
    int job_pid = vprocPid(job);
    assert(vprocSetPgid(job_pid, job_pid) == 0);
    hand_over_from_nobody(shell_pid, job_pid);
    assert(slave->tty->fg_group == job_pid);
    /* And a later thread of the tab leaves the terminal where it is. */
    vprocSessionStdioActivate(session);
    assert(slave->tty->session == shell_pid);
    assert(slave->tty->fg_group == job_pid);
}

typedef struct {
    const char *name;
    void (*run)(void);
} test_case;

#define CASE(fn) {#fn, fn}

int main(void) {
    static const test_case cases[] = {
        CASE(case_raw_every_key_one_at_a_time),
        CASE(case_raw_burst_stays_in_order),
        CASE(case_second_open_of_the_terminal),
        CASE(case_line_waits_for_return_and_echoes),
        CASE(case_line_every_key_of_many_lines),
        CASE(case_line_one_line_per_read),
        CASE(case_line_erase_key),
        CASE(case_line_end_of_file_key),
        CASE(case_prompt_ignores_old_end_of_file_key),
        CASE(case_line_too_long_for_the_buffer),
        CASE(case_line_control_keys_get_past_the_line),
        CASE(case_control_keys_stay_data_without_session_reader),
        CASE(case_interrupt_key_ends_a_waiting_read),
        CASE(case_interrupt_key_takes_lines_typed_ahead),
        CASE(case_interrupt_key_leaves_raw_mode_input_open),
        CASE(case_interrupt_key_stays_in_its_tab),
        CASE(case_prompt_takes_the_interrupt_key),
        CASE(case_waiting_reader_follows_change_to_raw),
        CASE(case_waiting_reader_follows_change_to_line),
        CASE(case_prompt_and_command_turns),
        CASE(case_poll_sees_lines_and_the_end_of_file_key),
        CASE(case_termios_in_linux_numbers),
        CASE(case_termios_in_the_hosts_numbers),
        CASE(case_dev_tty_is_the_tabs_terminal),
        CASE(case_dev_tty_follows_each_tab),
        CASE(case_dev_tty_reads_typed_lines),
        CASE(case_prompt_read_wakes_for_a_signal_that_ends_the_shell),
        CASE(case_reader_is_not_a_background_process),
        CASE(case_reader_is_not_refused_as_one),
        CASE(case_background_read_of_the_session_terminal),
        CASE(case_foreground_handoff_reaches_the_terminal),
        CASE(case_shell_claims_a_terminal_nobody_took),
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fflush(NULL);
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            /* Each case ends by leaving its threads behind. */
            alarm(30);
            cases[i].run();
            fflush(NULL);
            _exit(0);
        }
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid);
        bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        fprintf(stderr, "%s %s", passed ? "ok  " : "FAIL", cases[i].name);
        if (WIFSIGNALED(status)) {
            fprintf(stderr, " (signal %d)", WTERMSIG(status));
        }
        fprintf(stderr, "\n");
        if (!passed) {
            failed++;
        }
    }
    if (failed) {
        fprintf(stderr, "session terminal: %d failed\n", failed);
        return 1;
    }
    fprintf(stderr, "session terminal: all passed\n");
    return 0;
}
