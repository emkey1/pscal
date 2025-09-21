#!/usr/bin/env python3
import argparse, errno, os, signal, subprocess, sys, time


def _terminate_process_group(proc, sig):
    """Send *sig* to the process group for *proc*, handling platform quirks."""

    if proc is None:
        return

    pid = getattr(proc, "pid", None)
    if not pid or pid <= 0:
        return

    killpg = getattr(os, "killpg", None)
    if killpg is not None:
        try:
            killpg(pid, sig)
            return
        except ProcessLookupError:
            return
        except PermissionError:
            # Fall back to trying os.kill below.
            pass
        except OSError as e:
            if e.errno == errno.ESRCH:
                return
            # Unknown error: fall back to os.kill below.
            pass

    if os.name == "posix":
        try:
            os.kill(-pid, sig)
            return
        except ProcessLookupError:
            return
        except PermissionError:
            pass
        except OSError as e:
            if e.errno == errno.ESRCH:
                return

    try:
        proc.send_signal(sig)
    except ProcessLookupError:
        pass


def main():
    p = argparse.ArgumentParser(description="Run a command with a timeout, killing it if it hangs.")
    p.add_argument('--timeout', type=float, default=20.0, help='Timeout in seconds (default: 20)')
    p.add_argument('cmd', nargs=argparse.REMAINDER, help='Command to run')
    args = p.parse_args()

    if not args.cmd:
        print("No command specified", file=sys.stderr)
        return 2

    # Start the process in a new process group so we can kill children too.
    try:
        proc = subprocess.Popen(args.cmd, start_new_session=True)
    except TypeError:
        preexec_fn = getattr(os, "setsid", None)
        try:
            if preexec_fn is not None:
                proc = subprocess.Popen(args.cmd, preexec_fn=preexec_fn)
            else:
                proc = subprocess.Popen(args.cmd)
        except Exception as e:
            print(f"Failed to start: {e}", file=sys.stderr)
            return 127
    except Exception as e:
        print(f"Failed to start: {e}", file=sys.stderr)
        return 127

    deadline = time.time() + args.timeout
    exit_code = None
    try:
        while time.time() < deadline:
            exit_code = proc.poll()
            if exit_code is not None:
                break
            time.sleep(0.1)
        if exit_code is None:
            # Timeout: terminate process group
            _terminate_process_group(proc, signal.SIGTERM)
            # Give it a moment to exit
            deadline2 = time.time() + 2.0
            while time.time() < deadline2:
                exit_code = proc.poll()
                if exit_code is not None:
                    break
                time.sleep(0.05)
            if exit_code is None:
                _terminate_process_group(proc, signal.SIGKILL)
                exit_code = 124  # timeout
    except KeyboardInterrupt:
        _terminate_process_group(proc, signal.SIGKILL)
        raise

    return exit_code if exit_code is not None else 0

if __name__ == '__main__':
    sys.exit(main())

