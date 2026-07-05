# VM performance baseline suite

Phase 0 item 2 of the VM 2.0 plan ([Docs/pscal_vm2_plan.md](../../Docs/pscal_vm2_plan.md) §4):
a small, repeatable benchmark set whose numbers are recorded per phase, so
wins and regressions are attributed to the phase that caused them.

All benchmarks are written in Pascal (the most direct frontend onto the VM)
and self-time their workload with `RealTimeClock()`, so the reported number
excludes compile/startup time.  Each prints a deterministic `check=` value;
the runner fails loudly on a mismatch, because a changed check means the VM's
semantics changed and the timing is not comparable.

| Benchmark | Exercises |
|-----------|-----------|
| `arith.p` | tight arithmetic loop on locals: dispatch + int/real opcode cost |
| `calls.p` | naive Fibonacci + mutual recursion: CALL/RETURN, frames, params |
| `strings.p` | concat/copy/pos/compare churn: string alloc, copy, free |
| `globals.p` | 8 globals read+written per iteration: GET_GLOBAL/SET_GLOBAL (Phase 2b baseline) |
| `json.p` | repeated yyjson parse + full walk: ext-builtin dispatch + handle layer |
| `io_http.p` | text-file write/read rounds + HttpRequest over `file://` (loopback, no network) |

Each benchmark targets roughly 1 s on an M4 MacBook Pro so run-to-run noise
stays small relative to the signal.

## Running

```sh
python3 Tests/vm_bench/run_vm_bench.py                     # all, 5 runs each
python3 Tests/vm_bench/run_vm_bench.py --runs 9
python3 Tests/vm_bench/run_vm_bench.py --only arith,globals
python3 Tests/vm_bench/run_vm_bench.py --label "phase1b-psb3"
python3 Tests/vm_bench/run_vm_bench.py --no-record         # experiment only
```

The runner invokes `build/bin/pascal --no-cache <bench>` (override with
`--bin`), takes the **median** self-timed seconds over N runs, and appends
one JSON line to [history.jsonl](history.jsonl) with the UTC date, PBuild /
pscal-core / pascal git SHAs (`-dirty` suffixed when the tree is modified),
host, and per-benchmark median/min/max.  `history.jsonl` is committed —
that file *is* the per-phase record.

## Per-phase protocol

1. Land the phase, rebuild `build/bin/pascal`.
2. Run the suite on the **same host** as the previous entries
   (numbers are only comparable within a host) with a `--label`
   naming the phase, e.g. `--label "phase4-value-repr"`.
3. Commit the updated `history.jsonl` together with the phase.
4. If any benchmark reports `CHECK-MISMATCH`, the phase changed observable
   behavior — that's a correctness bug to fix first (see the differential
   harness, Phase 0 item 1), not a performance data point.

Changing a benchmark's workload constants changes its `check=` value and
breaks history continuity: update the expected value in `run_vm_bench.py`,
and treat older history rows for that benchmark as a separate series.

## Verifier load-time overhead (Phase 1e)

The benchmarks above self-time via `RealTimeClock()`, which only starts
*after* the chunk is already loaded — so they never see chunk-loading or
verification cost at all. `bench_verify_overhead.py` measures that
separately: whole-process wall-clock time for a cache-hit run of `calls.p`
with the Phase 1e verifier on vs. off (`PSCAL_VM_SKIP_VERIFY=1`), appending
to [verify_overhead_history.jsonl](verify_overhead_history.jsonl).

```sh
python3 Tests/vm_bench/bench_verify_overhead.py --bin build-release/bin/pascal
```

Baseline (2026-07-04, Release build, MacBook-Pro-2): ~194ms median
process time either way; verifier on/off delta ~1.8ms, smaller than
run-to-run jitter (~10-20ms) -- not distinguishable from noise, i.e.
negligible.
