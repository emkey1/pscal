#!/usr/bin/env python3
"""A VM thread that ends in raw mode puts the terminal back.

KeyPressed and ReadKey take the terminal out of canonical mode. A program that ends normally
puts it back in its exit cleanup; a killed one, and a spawned worker (which ends only as its
pool shuts down), leave that to a pthread key destructor. On Darwin a thread's _Thread_local
values already read as zero there, so the destructor never saw that its thread was in raw mode,
and the terminal was left with no echo and no line editing. This reads the terminal from
outside, after the kill.

    test_thread_end_raw_mode.py <exsh>      the iOS-flavoured shell, one process for all
"""
import os
import pty
import shutil
import subprocess
import sys
import tempfile
import termios
import threading
import time

WORKER = """void w() {
    keypressed();
}
int main() {
    int t = spawn w();
    join t;
    printf("joined\\n");
    fflush();
    delay(60000);
    return 0;
}
"""

MAIN = """int main() {
    keypressed();
    printf("raw\\n");
    fflush();
    delay(60000);
    return 0;
}
"""

# Each case: a script that touches mark when the terminal must be canonical again. It has no
# grep: the shell's root is the case's directory.
CASES = [
    # The program is killed while its one thread is in raw mode.
    ("killed", "clike m.clike > out1 &\np=$!\nwhile [ ! -s out1 ]; do sleep 0.1; done\n"
               "kill $p\nwait $p\n: > mark\nsleep 60\n"),
    # A worker thread enabled raw mode and went back to its pool; the program is killed.
    ("worker", "clike w.clike > out1 &\np=$!\nwhile [ ! -s out1 ]; do sleep 0.1; done\n"
               "kill $p\nwait $p\n: > mark\nsleep 60\n"),
]


def canonical(fd):
    lflag = termios.tcgetattr(fd)[3]
    return (lflag & (termios.ICANON | termios.ECHO)) == (termios.ICANON | termios.ECHO)


def run_case(exe, name, script):
    base = tempfile.mkdtemp(prefix="exsh-raw-mode.")
    try:
        with open(os.path.join(base, "w.clike"), "w") as f:
            f.write(WORKER)
        with open(os.path.join(base, "m.clike"), "w") as f:
            f.write(MAIN)
        with open(os.path.join(base, "t.sh"), "w") as f:
            f.write(script)
        master, slave = pty.openpty()
        env = dict(os.environ, HOME=base, TMPDIR=base, PATH_TRUNCATE=base)

        def child_setup():
            os.setsid()
            import fcntl
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        proc = subprocess.Popen([exe, "--no-cache", "t.sh"], cwd=base, env=env,
                                stdin=slave, stdout=slave, stderr=slave,
                                preexec_fn=child_setup)
        output = []

        def drain():
            while True:
                try:
                    data = os.read(master, 4096)
                except OSError:
                    return
                if not data:
                    return
                output.append(data)

        threading.Thread(target=drain, daemon=True).start()
        before = canonical(slave)
        mark = os.path.join(base, "mark")
        deadline = time.time() + 30
        while not os.path.exists(mark) and time.time() < deadline and proc.poll() is None:
            time.sleep(0.05)
        reached = os.path.exists(mark)
        # A destructor runs after the thread has said it is done.
        time.sleep(0.5)
        after = canonical(slave)
        proc.kill()
        proc.wait()
        os.close(slave)
        os.close(master)
        if not before:
            print(f"FAIL {name}: the terminal was not canonical to begin with")
            return False
        if not reached:
            text = b"".join(output).decode(errors="replace")
            print(f"FAIL {name}: the script never reached its mark\n{text}")
            return False
        if not after:
            print(f"FAIL {name}: the terminal is still in raw mode after the thread ended")
            return False
        print(f"ok {name}")
        return True
    finally:
        shutil.rmtree(base, ignore_errors=True)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    exe = os.path.abspath(sys.argv[1])
    results = [run_case(exe, name, script) for name, script in CASES]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
