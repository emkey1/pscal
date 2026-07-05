# VM thread-safety stress test

VM 2.0 Phase 2b (plan Docs/pscal_vm2_plan.md §5.7): a targeted stress test
for the `Value globals[]` / `chunk->global_slots[]` migration's cross-thread
locking discipline, since replacing `vmGlobalSymbols`/`vmConstGlobalSymbols`
HashTable lookups with direct array indexing is exactly the kind of change
that can silently drop a lock somewhere.

`globals_concurrency.pas` spawns six worker threads (`spawn`/`join`, backed
by `THREAD_CREATE`/`THREAD_JOIN`) that concurrently read and write ten
shared globals through `GET_GSLOT`/`SET_GSLOT` with **no mutex** on those
ten (only a separate shared counter is mutex-protected, as a control that
must always come out exactly right). The unprotected globals intentionally
tolerate the same benign races the pre-2b `GET_GLOBAL` fast path already
tolerated; what this actually checks is that `chunk->global_slots[]` itself
— the array all six threads index concurrently — never produces a crash,
an out-of-bounds access, or a use-after-free `Symbol*` under concurrent
`GET_GSLOT`/`SET_GSLOT` traffic. A locking regression should show up as a
sanitizer report or a hang, not a "wrong number" (only the mutex-protected
`counter` is asserted exactly; the ten unprotected globals are not).

## Running

```sh
# Build under ASan+UBSan first (build-asan/, see the repo's build docs).
build-asan/bin/pascal --no-cache Tests/vm_thread_stress/globals_concurrency.pas

# Repeat many times -- a race is a probabilistic thing to trigger.
for i in $(seq 1 100); do
  build-asan/bin/pascal --no-cache Tests/vm_thread_stress/globals_concurrency.pas \
    | grep -q "OK counter=10000" || echo "FAILED run $i"
done
```

Verified (2026-07-05): 150 consecutive clean runs under `build-asan`
(ASan+UBSan), 0 sanitizer reports, 0 hangs, `counter=10000` exactly every
time.

TSan was not set up for this repo as of this writing (no CMake toggle
exists yet); the ASan-repeated-run approach above is the documented
fallback per the phase's own rigor bar. If a TSan build ever gets added,
point it at the same fixture.
