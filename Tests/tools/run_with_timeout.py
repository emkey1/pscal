#!/usr/bin/env python3
import argparse, os, signal, subprocess, sys, time

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
        proc = subprocess.Popen(args.cmd, preexec_fn=os.setsid)
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
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            # Give it a moment to exit
            deadline2 = time.time() + 2.0
            while time.time() < deadline2:
                exit_code = proc.poll()
                if exit_code is not None:
                    break
                time.sleep(0.05)
            if exit_code is None:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                exit_code = 124  # timeout
    except KeyboardInterrupt:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        raise

    return exit_code if exit_code is not None else 0

if __name__ == '__main__':
    sys.exit(main())

