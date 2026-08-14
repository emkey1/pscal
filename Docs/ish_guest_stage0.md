# Stage 0 — PSCAL as guest binaries under iSH-AOK

**Verdict: the strategy survives.** None of the three things that would have
killed it happened. PSCAL's desktop configuration cross-builds for aarch64
Linux with no source changes, every frontend runs under iSH-AOK's aarch64 JIT,
and the test suites produce the same verdicts in-guest as on the macOS desktop
— with exactly one exception, which is a PSCAL-side race, not an iSH bug.
Hello-world end-to-end latency in-guest is **46 ms**, against a 300 ms budget.

No iSH-AOK kernel, JIT, or exec-path change was needed or made. No change was
made to PSCAL's iOS app or vproc.

---

## 1. What was built

| Component | How |
|---|---|
| `pascal`, `clike`, `exsh`, `rea`, `aether` | PSCAL umbrella CMake, `-DPSCAL_FORCE_IOS=OFF -DSDL=OFF`, in a native `linux/arm64` `debian:trixie-slim` container |
| `smallclue` | its own standalone build system (`fetch_dependencies.sh` + `build_smallclue.sh`), `debian:bookworm-slim` |
| guest rootfs | `iSH-AOK/tools/build-devuan-minirootfs.sh`, `ARCHES=arm64` |

The five frontends were built **from the local working tree**, not from GitHub
clones as `tools/build_pscal_aarch64_rootfs.sh` does, so the in-guest results
are diffable against a desktop baseline compiled from byte-identical sources.
The source was bind-mounted read-only and all artifacts landed in
container-local storage.

The configure step needed **no patches, no shims and no toolchain file** — the
claim that the desktop build already targets real POSIX held up exactly as
stated. `PSCAL_PLATFORM_IOS` is off, vproc is not compiled, and the frontends
link against real glibc `fork`/`exec`/`waitpid`.

Result: aarch64 PIE ELF, dynamically linked against glibc 2.41
(`interpreter /lib/ld-linux-aarch64.so.1`). Dynamic linking was chosen over
static deliberately: it exercises iSH's ELF interpreter path and is what the
eventual "PSCAL packages coexisting with apt packages" product actually looks
like. `smallclue` is statically linked (its own build system's default).

### Environment

| | |
|---|---|
| Host | macOS 26.5.2, Apple M5, 10 cores |
| iSH-AOK | worktree HEAD `f6fe2d30`, `ish` CLI rebuilt from that commit |
| PSCAL | `548d98ce`, all 12 submodules clean |
| Guest | Devuan 6 "excalibur" aarch64, glibc 2.41, gcc 14.2, Python 3.13.5 |
| Emulation | iSH-AOK 5.20.66, **4 emulated CPUs**, arm64 gadget JIT |

Guest invocation is the fast local loop, not a device:

```bash
./build/ish -f /Users/mke/pscal-ish-stage0/roots/pscal-devuan-arm64 /bin/bash -c '...'
```

---

## 2. Test results: desktop vs in-guest

`Tests/run_all_tests` is `set -e`, so the first sub-suite that exits nonzero
aborts every later one. That happened on **both** sides (both died in the exsh
section), which would have left Rea/Tiny/pscalasm/examples untested on both.
Each sub-suite was therefore run individually.

| Suite | Desktop pass/fail | Guest pass/fail | Same verdict? |
|---|---|---|---|
| pascal | 122 / 0 (3 skip) | 122 / 0 (3 skip) | yes |
| clike | 96 / 0 (4 skip) | 95 / 0 (5 skip) | yes — extra skip is `clike_graphics` (SDL off) |
| exsh | 110 / 6 | 115 / 1 | see §3.1, §3.2 |
| exsh_env_snap | 0 / 0 | (no compiler in first root) | closed, see §4 |
| rea | 77 / 0 | 75 / 0 (2 skip) | yes — 2 extra skips are `balls3d*` (SDL off) |
| tiny | 3 / 0 | (missing `tools/` in first root) | closed, see §4 |
| pscalasm | 11 / 10 | 11 / 10 | yes — identical failure set |
| examples | 27 / 0 | 27 / 0 | yes |
| **Total** | **446 / 16** | **445 / 11** | |

The guest column above is the first full pass, run on a deliberately lean root.
Two rows are blanks caused by that root rather than by results, and the
`pscalasm` row is environmental on both sides; §4 re-runs all three on a
richer root and closes them.

`Tests/examples/compile_all_examples.py`:

| | Desktop | Guest |
|---|---|---|
| passed | 189 | 144 |
| failed | 4 | 4 |
| failure set | \<identical\> | \<identical\> |

The 45-example gap is **entirely** `*/sdl/` examples, which
`compile_all_examples.py` auto-skips when `build/CMakeCache.txt` says SDL is
off. Set difference computed explicitly: 45 differing entries, **0** of them
outside `/sdl/`, and **0** examples that pass in-guest but not on desktop.

The 4 shared example failures (`docs_examples/{CheckFile,Demo,ReverseWord,
ThreadingConfig}`) are a pre-existing PSCAL packaging inconsistency, identical
on both sides: the binaries search `/usr/local/pscal/pascal/lib` while
`cmake --install` writes `/usr/local/pscal/lib/pascal`. Unrelated to iSH.

The 10 shared `pscalasm` failures are the runner shelling out to `rg`
(ripgrep), which is absent from the macOS host. Also unrelated to iSH.

---

## 3. Findings

### 3.1 The only genuine in-guest behavioural difference — and it is a PSCAL bug

`exsh` test **`thread_worker_reuse`** ("Worker pool reuses idle threads") fails
**10/10 in-guest** and **0/10 on desktop**. Deterministic, not flaky, and it
fails on the very first of the 18 iterations:

```
Thread 1 is still running; join it before retrieving the result.
```

`WaitForThread` returns before a worker with a 10 ms delay has finished.
Minimal probes in-guest:

| Probe | Result |
|---|---|
| spawn `delay 10`, `WaitForThread`, get result | **fails** |
| spawn `delay 10`, `WaitForThread`, `sleep 1`, get result | passes |
| spawn `delay 0`, `WaitForThread`, get result | passes |

**This is not an iSH threading bug.** A standalone C probe with no PSCAL
involved (`pthread_create` → child sleeps → publishes flag + 256-word payload →
parent `pthread_join` → check) run in the guest across 450 iterations:

```
join/no-sleep     iters=200 sleep=0us      join_not_done=0 payload_corrupt=0 join_mean=0.30ms
join/10ms-sleep   iters=200 sleep=10000us  join_not_done=0 payload_corrupt=0 join_mean=15.36ms
join/50ms-sleep   iters=50  sleep=50000us  join_not_done=0 payload_corrupt=0 join_mean=58.43ms
RESULT: PASS (join synchronized every iteration)
```

`pthread_join` blocks for the full duration and carries the required
happens-before on every iteration, on 4 emulated CPUs.

The actual cause is a startup race in PSCAL's own thread pool, in
`components/pscal-core/src/vm/vm.c`:

```c
static bool joinThreadInternal(VM* vm, int id) {
    ...
    while (!thread->statusReady) {
        if (!atomic_load(&thread->active) && !thread->awaitingReuse) {
            pthread_mutex_unlock(&thread->resultMutex);
            return false;           // gives up instead of waiting
        }
```

If the joiner reaches this before the just-spawned worker has marked itself
`active`, then `statusReady`, `active` and `awaitingReuse` are all false and it
returns `false` **without waiting at all**. Its caller then discards that:

```c
bool vmJoinThreadById(VM* vm, int id) {
    joinThread(vm, id);   // return value ignored
    ...
    return true;          // unconditional
}
```

So `WaitForThread` reports success on a join that never happened, and the
subsequent `ThreadGetResult` correctly complains the thread is still running.
A fast native host wins this race; iSH loses it every time. **This is a real
PSCAL bug that iSH merely exposes** — it is equally reachable on a loaded or
slow native machine.

### 3.2 exsh's bash-parity failures are a macOS artifact, not a PSCAL defect

The guest **passes** the 6 exsh tests the desktop fails (`declare_global_scope`,
`mapfile_basic`, `bash_parity_{declare_assoc,bind_shopt,kill,set_posix}`).
Those tests diff exsh's behaviour against the system `bash`; macOS ships bash
3.2, which has no `declare -g` and no `mapfile`, so the *reference* fails, not
exsh. Against Devuan's bash 5.x all six pass in-guest, confirmed on both roots.
Useful side effect: **the iSH guest is a better bash-parity oracle for exsh than
the macOS host is** — worth wiring into CI regardless of what happens to the
rest of this plan.

### 3.3 No syscall gap

iSH reports unimplemented syscalls via `printk`, which in the CLI build goes to
**fd 555** (`LOG_HANDLER_DPRINTF`, `kernel/log.c`) — *not* stderr, so grepping a
normal run's output for them proves nothing.

With fd 555 captured across a full suite run (all nine sub-suites, all five
frontends, ~340 tests), the entire kernel log is **one line**:

```
[Fri Aug 14 16:34:51 2026] iSH-AOK 5.20.66-ish_aok built Aug 14 2026 16:18:26 booted on 4 emulated CPU(s)
```

Zero `missing ... syscall` reports. The capture is demonstrably live — it
recorded the boot banner — so this is a real negative, not a silent one.
**PSCAL's frontends hit no syscall gap in iSH-AOK.**

### 3.4 No JIT correctness divergence under the bytecode-VM dispatch loop

This was flagged as the real unknown, since iSH's arm64 JIT is mostly exercised
by shell/coreutils workloads rather than a tight interpreter loop. Every
non-SDL test that ran on both sides produced the same verdict, with §3.1 the
only exception and that one root-caused off the JIT. The suites compare exact
stdout, so this is byte-level agreement across ~445 tests and 144 compiled
example programs spanning all four language frontends plus the VM.

`ISH_ARM64_FORCE_INTERP=1` was considered as a JIT-vs-interpreter A/B oracle
and **not** used: `jit/jit.c` documents it as an unmaintained bisection escape
hatch that "will crash on anything nontrivial", so a disagreement would not be
attributable. The suite agreement above is the stronger evidence.

### 3.5 PSCAL build-system gaps that will matter later (not blockers now)

- **`smallclue` has no Linux target in the umbrella.**
  `add_executable(smallclue ...)` is guarded by `if(PSCAL_PLATFORM_IOS OR
  APPLE)`, so a Linux configuration of the umbrella silently has no such
  target. It had to be built from its own standalone build system.
- **The Linux `exsh` has no smallclue applet builtins.** On macOS the applets
  are linked into exsh (`smallclueGetApplets()`); on Linux they are not, so
  `watch` falls through to `PATH`. That is what originally aborted the whole
  in-guest exsh suite: Devuan's procps `watch` treats `-c` as `--color` and has
  no count flag, so `watch -n 0.05 -c 1 foo` loops forever and hit the harness's
  20 s timeout. macOS has no `/usr/bin/watch` at all, so the desktop never sees
  this. For a Devuan-based PSCAL variant where SmallCLUE applets are meant to
  coexist with apt packages, this shadowing question is exactly the thing
  `dpkg-divert` has to settle.
- `smallclue`'s standalone CMake adds `third-party/nextvi/vi.c` and the vendored
  OpenSSH tree to `SOURCES` unconditionally; with those submodules unpopulated
  it fails at `add_executable` with the misleading "No SOURCES given to target".
  Its OpenSSH also does not configure against trixie's OpenSSL 3.5 ("working
  libcrypto not found") — bookworm works.

---

## 4. Coverage gaps closed on the second pass

The first guest root was too thin in two ways, both mine and neither iSH's:
`run_tiny_tests.sh` needs `$REPO_ROOT/tools/tiny` (I had not staged `tools/`),
and `run_exsh_env_snapshot_tests.sh` needs `cc`. The root was rebuilt with
`gcc`, `libc6-dev` and `ripgrep`, and `tools/` staged.

On that second root the two gaps closed and one earlier result was explained
outright:

| Suite | Desktop | Guest (second root) | Note |
|---|---|---|---|
| tiny | 3 / 0 | **3 / 0** | gap closed |
| pscalasm | 11 / 10 | **21 / 0** | guest has `ripgrep`; confirms all 10 desktop failures were the missing `rg`, nothing to do with iSH or with pscalasm |
| exsh | 110 / 6 | **115 / 1** | reproduced exactly across both roots; the 1 is §3.1 |
| exsh_env_snap | 0 / 0 (passes) | build error | see below |

`run_exsh_env_snapshot_tests.sh` compiles `test_env_snapshot_restore.c` with
`cc` and now fails on **gcc 14**, not on anything iSH does:

```
error: implicit declaration of function 'unsetenv'; did you mean 'getenv'?
```

gcc 14 makes implicit declarations an error by default. The test source needs
`#include <stdlib.h>` under the right feature-test macros; Apple clang accepts
it today. A PSCAL portability item, filed here rather than fixed, since Stage 0
is not supposed to change PSCAL.

### One unreproduced event

In the traced run, `clike_MStreamWords` failed once: it printed the first line
of expected output and then exited **241** without printing the second. That
fixture loads `etc/words` (23 KB, 3135 lines) into a memory stream and walks it
character by character — a long, tight VM loop, i.e. exactly the shape that
would expose a JIT bug if one existed.

It did not reproduce: **0 failures in 130 targeted retries** in-guest (30 + a
100-run stress), and it passed in the other two full guest runs. Desktop is
20/20 clean. Recorded here rather than dismissed,
because a single unexplained abnormal exit inside the VM dispatch loop is the
one observation in this whole exercise that is *consistent* with kill-criterion
(b) — but on the evidence available it is a one-off, and everything else about
the JIT came back clean.

---

## 5. Measurements

Seven runs per case, first discarded as warm-up, machine otherwise quiet.
Identical `bench.py` on both sides, timing wall-clock around process spawn, so
every sample includes **fork/exec + dynamic link + compile + run**.

Median ms:

| Case | Host (native macOS) | Guest (HLE off) | Guest / host | Guest (HLE on) |
|---|---|---|---|---|
| `/usr/bin/true` (spawn floor) | 1.6 | 3.8 | 2.4× | 5.3 |
| `aether -v` | 16.3 | 34.8 | 2.1× | 51.1 |
| **`aether --no-cache hello` (hello-world e2e)** | **18.0** | **46.1** | **2.6×** | 83.1 |
| `aether --no-cache bitwise_ops` | 19.3 | 60.6 | 3.1× | 190.0 |
| `aether --no-cache --dump-bytecode-only bitwise_ops` (compile only) | 20.0 | 66.2 | 3.3× | 157.6 |
| `pascal --no-cache hello` | 17.7 | 42.8 | 2.4× | 73.1 |

Spread was tight (e.g. guest hello-world min 45.1 / median 46.1 / max 49.5).

### Hello-world end-to-end latency: 46 ms

Against the ~300 ms threshold that "changes the plan", this passes with ~6.5×
headroom. Process spawn plus dynamic link accounts for only about 3.8 ms of it;
the rest is PSCAL's own startup and compile, which is also the bulk of the
17.7–18.0 ms the host spends. iSH is not the dominant cost.

### `aether --no-cache` compile time

66.2 ms in-guest vs 20.0 ms native for the same source, i.e. **3.3×**. Whole-
program throughput sits in the same 2.4–3.3× band. That is much better than the
~10× seen while running the full suites, where per-test process spawning and
Python harness overhead dominate rather than PSCAL's own work.

### HLE makes this *slower* — leave it off

`ISH_HLE=1` is a consistent regression here: hello-world 46.1 → 83.1 ms
(+80%), and `bitwise_ops` 60.6 → 190.0 ms (+214%). HLE replaces hot libc
routines with native code and pays a symbol-attach cost at startup; for
short-lived compile-and-exit processes that cost is never amortised. This
matches `jit_code_cache_plan.md`'s note that HLE did not help cold start and
cost +15–30% there, though the penalty measured here is considerably larger.

HLE was verified to be genuinely engaged, not silently inert —
`ISH_HLE_STATS=1` on a single `aether hello`:

```
hle stats:   size 1k-4k    760
hle stats:   size 4k-64k   15
hle stats:   size unsized  26223
```

That distribution is the explanation: ~26k of the ~27k intercepted calls are
small/unsized, where the native-call overhead is never repaid, and only 15 land
in the 4k–64k range where HLE actually wins. Nothing in the PSCAL workload is
dominated by the large `memcpy`/`strlen`-shaped loops HLE exists for.

---

## 6. What would still need answering

Stage 0 deliberately did not cover these:

- **Device hardware.** Everything here is `ish-cli` on an M5. An A-series iPad
  or older iPhone will be slower; the 46 ms figure is a floor, not a promise.
  The 2.4–3.3× emulation ratio should transfer, but absolute latency will not.
- **SDL/graphics.** Built with `SDL=OFF`, so 45 examples and 3 fixtures went
  untested in-guest.
- **The dpkg-divert coexistence question** (§3.5) is untouched, and the `watch`
  collision shows it is real.
- **Cold-cache behaviour.** All measurements used `--no-cache`, which is the
  honest compile-time number, but the bytecode-cache hit path was not measured
  in-guest.
- The §3.1 race should be fixed in PSCAL regardless of iSH.

---

## Reproducing

Scripts used for this pass live outside both repos, in
`/Users/mke/pscal-ish-stage0/`:

| Script | Does |
|---|---|
| `build_linux_aarch64.sh` | five frontends + `DESTDIR` install tree, from the local tree |
| `build_smallclue.sh` | smallclue via its own standalone build system |
| `assemble_root.sh` | Devuan tar + install tree + trimmed PSCAL source → `fakefsify` root |
| `run_suites_individually.sh` | per-sub-suite runner (works unchanged on host and guest) |
| `run_guest_with_trace.sh` | same, with iSH's kernel log captured off fd 555 |
| `bench.py` / `bench_all.sh` | the §5 measurements, host + guest ± HLE |
| `join_semantics.c` | the §3.1 `pthread_join` probe |
| `summarize.py` | turns a runner log into the §2 table |

`fakefsify` cannot copy a host file into an existing root, so anything injected
into the guest has to be appended to the tar *before* conversion — which is why
`assemble_root.sh` builds the whole image in one shot.
