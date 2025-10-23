# Threading and Worker Pool Guide

The PSCAL virtual machine exposes a shared worker pool so front ends can offload
bytecode routines, VM builtins, or host callbacks onto background threads. This
guide collects the conventions that apply across languages and shells so script
authors can plan thread names, size the pool, and interpret the metrics exposed
by `ThreadStats`.

## Thread naming conventions

Each worker keeps a printable identifier capped at `THREAD_NAME_MAX` (64)
characters. Names are truncated automatically when longer than the limit so
callers should place the most distinctive tokens first.【F:src/vm/vm.h†L31-L120】
When no name is supplied the runtime falls back to `worker-<slot>` using the
pool index, ensuring that reused slots remain recognisable in diagnostic output
and across generations.【F:src/vm/vm.c†L820-L877】 Wrapper helpers in the Pascal
`Threading` unit and language front ends translate friendly helper names into
these records so handle lookups stay portable across runtimes.【F:lib/pascal/threading.pl†L1-L54】

`ThreadStats` returns both the numeric `id` and the `pool_generation` counter, so
reused workers can be distinguished even if they are assigned the same name.
Fields such as `paused`, `cancel_requested`, and `ready_for_reuse` mirror the
flags stored in the VM’s thread table, allowing monitoring dashboards to mirror
what the runtime observes.【F:src/backend_ast/builtin.c†L5057-L5104】

## Pool sizing and environment overrides

The VM lazily grows the worker pool up to `VM_MAX_WORKERS` (15 workers plus the
main thread). New slots are only reserved when a job cannot reuse an idle worker
and the cap has not been reached.【F:src/vm/vm.c†L1319-L1358】 All front ends
honour the optional environment variables `PSCAL_THREAD_POOL_SIZE` (Pascal,
CLike, Rea, Tiny) and `PSCALSHELL_THREAD_POOL_SIZE` (exsh). Set either variable
to an integer between `1` and `VM_MAX_WORKERS` to clamp the pool. Values outside
the range are ignored and the VM falls back to its lazy allocator.

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
handle.【F:src/vm/vm.c†L1008-L1339】 Cancelling a worker merely raises
`cancelRequested`; long-running routines are expected to poll their pause state
(e.g. between iterations) and exit promptly when the flag is raised. Submitting
jobs with `submitOnly` keeps the caller’s slot available so interactive shells
can continue handling input while background jobs drain the queue.【F:src/backend_ast/builtin.c†L4930-L5199】

## Interpreting `ThreadStats`

`ThreadStats` returns an array of records with a stable schema. Each entry
includes lifecycle flags (`active`, `idle`, `status_ready`, `result_consumed`),
wall-clock timestamps (`queued_at`, `started_at`, `finished_at`), and a nested
`metrics` record. The `metrics.start` and `metrics.end` samples include CPU
runtime, microsecond-resolution `getrusage` totals, and resident-set estimates.
When a worker is still running, `ThreadStats` synthesises an `end` sample from
the current counters so monitors always receive up-to-date numbers.【F:src/backend_ast/builtin.c†L5008-L5104】
`ThreadMetrics` is part of the public VM surface, so embedders can collect the
same data without round-tripping through a builtin.【F:src/vm/vm.h†L64-L119】

For quick shell-level reporting, the `.exshrc` template wires a helper that
feeds `ThreadStats` into `pscjson2bc` or other text processors. Pascal, CLike,
and Rea code can call `ThreadStatsCount` (from the `Threading` unit) to gauge
pool pressure without materialising the full record array.【F:lib/pascal/threading.pl†L1-L54】【F:etc/skel/.exshrc†L118-L160】

## Manual verification and benchmarking

Manual smoke tests help confirm concurrency changes before committing to a full
bench run:

- Run `build/bin/exsh Examples/exsh/threading_demo` to confirm thread names,
  status handling, and cooperative cancellation behave as expected for the
  builtins allowed on worker threads.【F:Examples/exsh/threading_demo†L1-L30】
- Run `build/bin/exsh Examples/exsh/threading_showcase` to exercise
  `ThreadSpawnBuiltin`, `ThreadPoolSubmit`, naming/lookup helpers, result
  collection, and the `ThreadStats` snapshot in a single transcript before
  landing worker-pool changes.【F:Examples/exsh/threading_showcase†L1-L135】
- Execute `build/bin/pascal Examples/pascal/base/docs_examples/threading_config`
  after exporting `PSCAL_THREAD_POOL_SIZE=<n>` to verify Pascal observes the
  configured limit and reports worker usage through `ThreadStatsCount`.
- When adjusting pool sizing heuristics, compare throughput with Bash via the
  upstream `shellbench` harness. The project notes include recommended command
  lines and historical baselines for the `assign.sh`, `cmp.sh`, `eval.sh`,
  `null.sh`, `subshell.sh`, and `stringop1.sh` suites; keep exsh within a few
  percent of the tracked numbers while watching for `?` or `error` tallies that
  indicate control-flow regressions.【F:Misc/Benchmarks†L1-L160】

Capturing the same metrics before and after a change helps isolate whether a
regression stems from pool contention (increasing queue times) or worker reuse
bugs (slots never returning to the idle state).
