# iOS Resize Harness

`Tests/ios_vproc/test_resize_harness.c` validates the iOS resize pipeline at the vproc/runtime boundary:

1. Resize request is applied to the session PTY (`vprocSetSessionWinsize`).
2. Runtime-style signal dispatch is attempted (`pthread_kill(..., SIGWINCH)`).
3. Session and live TTY geometry match the requested rows/columns.
4. Deferred resize requests received before PTY/session bind are replayed when
   the PTY is attached.
5. Frontend-style `session=0` deferral mirrors hterm behavior: native resize
   events are deferred while unbound and the latest deferred size is replayed
   when a session is rebound.
6. Stale deferred frontend sizes are not allowed to clobber newer runtime
   session geometry after rebind.

The harness runs as part of `Tests/run_ios_vproc_tests.sh` as:

- `build/ios_vproc_resize_harness_test`

You can also run it directly:

```bash
./build/ios_vproc_resize_harness_test
```

Expected output includes lines like:

```text
[resize-harness] step=1 request=121x37 set_rc=0 signal_rc=0 session=9701
PASS ios resize harness
```

## Scope

This harness verifies resize requests are sent and applied correctly for iOS session PTYs.
It also includes lightweight editor-path probes:

- `nextvi`: runtime/PTY size detection path
- `micro`: embedded pipe/env fallback size detection path
- `hterm relay`: unbound resize deferral and rebind replay behavior
- `hterm relay stale-guard`: deferred replay is skipped if runtime already
  holds a non-default, newer size for that session

It does **not** validate full editor redraw behavior; that still needs on-device UI checks.

Important: this C harness links `src/ios/runtime_session_stub.c` in host CI, so it does not execute the full Objective-C runtime bridge (`PSCALRuntime.mm`) event path.

## Real-Environment Harness (Xcode Log Driven)

To validate the actual iOS/iPadOS end-to-end runtime chain from real device/simulator runs, use:

- `Tests/ios_vproc/resize_log_harness.py`

It parses Xcode console output and verifies:

1. `hterm runtime-forward` events are followed by `runtime updateSessionWindowSize`.
2. Runtime updates are followed by `vproc setSessionWinsize` (`applied` or `missing-pty` then resolved).
3. `runtime-defer` only occurs while unbound (`session=0`).
4. Dynamic size changes are observed per active session.

Usage:

```bash
python3 Tests/ios_vproc/resize_log_harness.py /path/to/xcode-console.log
```

Exit status:

- `0`: pass (pipeline consistent with expectations)
- `1`: fail (missing handoffs or stale unresolved resize states)
