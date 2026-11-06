#!/usr/bin/env python3
"""A VM program puts the terminal back as it found it, not as the first one found it.

The VM keeps the termios it saw before it took the terminal out of canonical mode, and goes back
to it at a canonical read and at the end. It saved that once for the whole process, and in the
app every command runs in one process: so after any earlier ReadKey program, every later one, on
any tab, put back that first program's settings, undoing an stty since. This runs one program,
changes the terminal from outside (ECHOCTL off), and checks the next program keeps the change,
at its ReadLn in the foreground and when it is killed in raw mode. A run that ends normally
doesn't show it: the shell puts back the termios it saw around every frontend run.

    test_termios_base.py <exsh>      the iOS-flavoured shell, one process for all
"""
import fcntl
import os
import pty
import shutil
import subprocess
import sys
import tempfile
import termios
import threading
import time

FIRST = """int main() {
    keypressed();
    return 0;
}
"""

KILLED = """int main() {
    keypressed();
    printf("raw\\n");
    fflush();
    delay(60000);
    return 0;
}
"""

READER = """int main() {
    str s;
    keypressed();
    printf("ready\\n");
    fflush();
    readln(stdin, s);
    return 0;
}
"""

# A script that waits for go after its first program and touches mark files the test waits on.
# It has no grep: the shell's root is the case's directory.
SCRIPT = """clike first.clike
: > mark1
while [ ! -e go ]; do sleep 0.1; done
clike reader.clike
: > mark2
clike killed.clike > out1 &
p=$!
while [ ! -s out1 ]; do sleep 0.1; done
kill $p
wait $p
: > mark3
sleep 60
"""


def echoctl(fd):
    return bool(termios.tcgetattr(fd)[3] & termios.ECHOCTL)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    exe = os.path.abspath(sys.argv[1])
    base = tempfile.mkdtemp(prefix="exsh-termios-base.")
    try:
        for name, text in (("first.clike", FIRST), ("reader.clike", READER), ("killed.clike", KILLED), ("t.sh", SCRIPT)):
            with open(os.path.join(base, name), "w") as f:
                f.write(text)
        master, slave = pty.openpty()
        attrs = termios.tcgetattr(slave)
        attrs[3] |= termios.ECHOCTL
        termios.tcsetattr(slave, termios.TCSANOW, attrs)
        env = dict(os.environ, HOME=base, TMPDIR=base, PATH_TRUNCATE=base)

        def child_setup():
            os.setsid()
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

        def wait_for(test, what):
            deadline = time.time() + 30
            while time.time() < deadline and proc.poll() is None:
                if test():
                    return True
                time.sleep(0.05)
            text = b"".join(output).decode(errors="replace")
            print(f"FAIL: the script never reached {what}\n{text}")
            return False

        failed = False
        try:
            if not wait_for(lambda: os.path.exists(os.path.join(base, "mark1")), "mark1"):
                return 1
            attrs = termios.tcgetattr(slave)
            attrs[3] &= ~termios.ECHOCTL
            termios.tcsetattr(slave, termios.TCSANOW, attrs)
            open(os.path.join(base, "go"), "w").close()
            if not wait_for(lambda: b"ready" in b"".join(output), "the reader's ReadLn"):
                return 1
            time.sleep(0.5)
            if echoctl(slave):
                print("FAIL foreground: at its ReadLn the second program put back the first one's terminal")
                failed = True
            else:
                print("ok foreground")
            os.write(master, b"x\n")
            if not wait_for(lambda: os.path.exists(os.path.join(base, "mark3")), "mark3"):
                return 1
            time.sleep(0.3)
            if echoctl(slave):
                print("FAIL killed: a killed program put back the first one's terminal")
                failed = True
            else:
                print("ok killed")
        finally:
            proc.kill()
            proc.wait()
            os.close(slave)
            os.close(master)
        return 1 if failed else 0
    finally:
        shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
