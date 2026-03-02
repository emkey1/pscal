# iOS Resize Harness

`Tests/ios_vproc/test_resize_harness.c` validates the iOS resize pipeline at the vproc/runtime boundary:

1. Resize request is applied to the session PTY (`vprocSetSessionWinsize`).
2. Runtime-style signal dispatch is attempted (`pthread_kill(..., SIGWINCH)`).
3. Session and live TTY geometry match the requested rows/columns.

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

It does **not** validate full editor redraw behavior; that still needs on-device UI checks.
