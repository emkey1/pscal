# Threading and Worker Pool Guide

The PSCAL virtual machine exposes a shared worker pool so front ends can offload
bytecode routines, VM builtins, or host callbacks onto background threads. The
enhanced task/pool model (naming, stats, pooling) is **intended for the exsh
shell front end**; other front ends still have basic worker support but should
not assume every exsh convenience is available. This guide collects the
conventions that apply across languages and shells so script authors can plan
thread names, size the pool, and interpret the metrics exposed by `ThreadStats`.

## Thread naming conventions

Each worker keeps a printable identifier capped at `THREAD_NAME_MAX` (64)
characters. Names are truncated automatically when longer than the limit so
callers should place the most distinctive tokens first.【F:src/vm/vm.h†L31-L120】
When no name is supplied the runtime falls back to `worker-<slot>` using the
pool index, ensuring that reused slots remain recognisable in diagnostic output
and across generations.【F:src/vm/vm.c†L952-L1005】 Wrapper helpers in the Pascal
`Threading` unit and language front ends translate friendly helper names into
these records so handle lookups stay portable across runtimes.【F:lib/pascal/threading.pl†L1-L54】

`ThreadStats` returns both the numeric `id` and the `pool_generation` counter, so
reused workers can be distinguished even if they are assigned the same name.
Fields such as `paused`, `cancel_requested`, and `ready_for_reuse` mirror the
flags stored in the VM’s thread table, allowing monitoring dashboards to mirror
what the runtime observes.【F:src/backend_ast/builtin.c†L5074-L5113】

Use `ThreadSetName(threadId, "label")` to assign human-readable identifiers to
workers and `ThreadLookup("label")` when you need to map those labels back to
handles from another part of the program. Both helpers fall back to the slot
owner when the caller executes inside an embedded VM so names remain visible to
all front ends.【F:src/backend_ast/builtin.c†L5677-L5729】

## Pool sizing and environment overrides

The VM lazily grows the worker pool up to `VM_MAX_WORKERS` (15 workers plus the
main thread). New slots are only reserved when a job cannot reuse an idle worker
and the cap has not been reached.【F:src/vm/vm.c†L1470-L1538】 All front ends
honour the optional environment variables `PSCAL_THREAD_POOL_SIZE` (Pascal,
CLike, Rea, Tiny) and `PSCALSHELL_THREAD_POOL_SIZE` (exsh). Set either variable
to an integer between `1` and `VM_MAX_WORKERS` to clamp the pool. Values outside
the range are ignored and the VM falls back to its lazy allocator.

These variables are read at process/VM startup; changing them after exsh (or an
embedded VM) has launched will not resize the active pool for that session.
Quick check: run `PSCALSHELL_THREAD_POOL_SIZE=3 build/bin/exsh -c "threadpoolsubmit delay 1 && threadstatsjson"` and confirm `pool_generation` entries show at most 3 workers. Values outside `1..15` are ignored and the pool grows lazily.

Interactive shells can export the shell-specific variable in `~/.exshrc`; the
repository template shows a commented example that also wires in a helper for
printing worker summaries.【F:etc/skel/.exshrc†L118-L160】 Pascal code can consult
`PSCAL_THREAD_POOL_SIZE` (or emit hints for operators) before queueing work; the
`threading_config` sample demonstrates a portable pattern for reading the
variable and reporting the configured cap alongside `ThreadStatsCount`.【F:Examples/pascal/base/docs_examples/threading_config†L1-L44】

## Cooperative cancellation and reuse

The worker loop respects `ThreadPause`, `ThreadResume`, `ThreadCancel`, and
`ThreadKill`, toggling per-thread atomics before resuming jobs or abandoning
assignments. Workers mark themselves `awaiting_reuse` once they have published a
result and stay parked until the caller consumes the status flag and releases
the slot, preventing accidental reuse while downstream code still holds a
handle.【F:src/vm/vm.c†L1014-L1047】【F:src/vm/vm.c†L1186-L1405】 Cancelling a worker merely raises
`cancelRequested`; long-running routines are expected to poll their pause state
(e.g. between iterations) and exit promptly when the flag is raised. Submitting
jobs with `submitOnly` keeps the caller’s slot available so interactive shells
can continue handling input while background jobs drain the queue.【F:src/backend_ast/builtin.c†L4946-L5431】

Safe vs. unsafe work on workers:
- Safe (exsh workers): pure computation; file/network I/O that is thread-safe in
  the host; VM builtins documented as thread-safe; DNS/HTTP helpers that do not
  touch terminal state.
- Avoid/unsafe (exsh workers): UI/SDL work; terminal/geometry updates
  (window-size calls, cursor rendering); host APIs that demand main-thread
  affinity (UIKit/SwiftUI bridges); builtins that mutate global state without
  locking (e.g., PATH_TRUNCATE/application-global env reshaping). These should
  stay on the foreground/main thread.
- Enforcement: exsh’s `threadpoolsubmit`/`threadspawnbuiltin` reject any
  builtin not on the thread-safe allowlist and set the last status to 1, so UI /
  terminal-bound operations are blocked from workers by default.

If a worker is cancelled or dies mid-job, queued work remains until a slot is
reused; consume `status_ready`/`result_consumed` promptly to avoid handle leaks,
and prefer cooperative cancellation so the worker can clean up.

## Interpreting `ThreadStats`

`ThreadStats` returns an array of records with a stable schema. Each entry
includes lifecycle flags (`active`, `idle`, `status_ready`, `result_consumed`),
wall-clock timestamps (`queued_at`, `started_at`, `finished_at`), and a nested
`metrics` record. The `metrics.start` and `metrics.end` samples include CPU
runtime, microsecond-resolution `getrusage` totals, and resident-set estimates.
When a worker is still running, `ThreadStats` synthesises an `end` sample from
the current counters so monitors always receive up-to-date numbers.【F:src/backend_ast/builtin.c†L5042-L5113】
`ThreadMetrics` is part of the public VM surface, so embedders can collect the
same data without round-tripping through a builtin.【F:src/vm/vm.h†L64-L119】

For quick shell-level reporting, the `.exshrc` template wires a helper that
feeds `ThreadStats` into `pscjson2bc` or other text processors. Pascal, CLike,
and Rea code can call `ThreadStatsCount` (from the `Threading` unit) to gauge
pool pressure without materialising the full record array.【F:lib/pascal/threading.pl†L1-L54】【F:etc/skel/.exshrc†L118-L160】
The default exsh rc also provides `threadpool` (Python-backed) to show id,
pool_generation, name, active/ready/status fields at a glance.

## Manual verification and benchmarking

Manual smoke tests help confirm concurrency changes before committing to a full
bench run:

- Run `build/bin/exsh Examples/exsh/threading_demo` to confirm thread names,
  status handling, and cooperative cancellation behave as expected for the
  builtins allowed on worker threads.【F:Examples/exsh/threading_demo†L1-L30】
- Run `build/bin/exsh Examples/exsh/parallel-check github.com example.com` to
  queue DNS probes in parallel, tag workers with `ThreadSetName`, and release
  cached status/result pairs via `ThreadGetResult(..., true)` while the shell
  keeps running foreground commands.【F:Examples/exsh/parallel-check†L1-L74】
- Run `build/bin/exsh Examples/exsh/context_smoke` to exercise nested `exsh`
  launches that each install an isolated shell runtime context.【F:Examples/exsh/context_smoke†L1-L5】
- Execute `build/bin/pascal Examples/pascal/base/docs_examples/threading_config`
  after exporting `PSCAL_THREAD_POOL_SIZE=<n>` to verify Pascal observes the
  configured limit and reports worker usage through `ThreadStatsCount`.
- When adjusting pool sizing heuristics, compare throughput with Bash via the
  upstream `shellbench` harness. The project notes include recommended command
  lines and historical baselines for the `assign.sh`, `cmp.sh`, `eval.sh`,
  `null.sh`, `subshell.sh`, and `stringop1.sh` suites; keep exsh within a few
  percent of the tracked numbers while watching for `?` or `error` tallies that
  indicate control-flow regressions.【F:Misc/Benchmarks†L1-L160】 Latest run:
  `tools/shellbench_cacheable/shellbench -s ../../build/bin/exsh,bash sample/assign.sh sample/cmp.sh sample/eval.sh sample/null.sh sample/subshell.sh sample/stringop1.sh`
  returned numeric counts across all cases (e.g., exsh `assign.sh: positional`
  ~84k/s vs bash ~437k/s; `subshell.sh: command subs` ~10.7k/s vs 1.6k/s).

Capturing the same metrics before and after a change helps isolate whether a
regression stems from pool contention (increasing queue times) or worker reuse
bugs (slots never returning to the idle state).

## Work plan (exsh threading/task model)

Status legend: [ ] pending, [~] in progress, [x] done

- [x] Clarify scope: enhanced pool/task model is exsh-only; other front ends keep basic workers.
- [x] Catalog safe vs. unsafe builtins/host ops for exsh workers; document and gate where needed (worker allowlist enforced).
- [x] Enforce main-thread affinity for UI/SDL/terminal-bound operations; add guards in exsh builtins (blocked by allowlist).
- [x] Harden cancellation/cleanup paths: ensure `status_ready`/`result_consumed` are cleared and slots return to idle; add tests (threadpool_smoke).
- [x] Verify env overrides are startup-only and add regression notes for pool sizing/env bounds (documented quick check).
- [x] Add shell-level diagnostics: a concise `threadpool` helper in exsh showing names, generation, and ready/status fields.
- [x] Benchmark/smoke tests: `Examples/exsh/threading_demo`, `threadpool_smoke`, and `parallel-check` all pass (the DNS run requires network access to resolve hosts); shellbench rerun via `tools/shellbench_cacheable/shellbench -s ../../build/bin/exsh,bash ...` with all benchmarks returning numeric counts.
- [x] Per-VM shell context scaffold: allocate/free shell runtime state per VM, swap contexts when a VM is active, and ensure nested shell calls reuse the caller’s context so each exsh instance stays isolated.
- [x] Migrate remaining global shell state (jobs/arrays/traps/history) into the per-VM context and expose multi-instance entry points for iOS multi-window shells/background tasks.  Shell tool threads now install an isolated shell runtime context before running `exsh_main`, and new helpers `shellRuntimeActivateContext`/`shellRuntimeCurrentContext` let the iOS host select a context per window.
