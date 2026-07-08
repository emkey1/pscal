# PSCAL Virtual Machine Technical Manual

## Chapter 4: Built-in Subsystems & Native Bindings

> Source of truth for this chapter:
> `components/pscal-core/src/backend_ast/builtin.c` (the registry),
> `src/ext_builtins/` (the extension categories and their registration),
> `src/backend_ast/builtin_network_api.c` (HTTP/TLS/sockets, 3665 lines),
> `src/ext_builtins/sqlite/sqlite_builtins.c`, and
> `src/ext_builtins/openai/openai_chat.c`.

### 4.0 The Extensibility Model: One Registration, Every Frontend

The single most architecturally consequential property of PSCAL's builtin
layer is that **the VM is extensible through builtins, and every extension is
automatically inherited by every frontend language**. Pascal, Rea, CLike,
Aether, and exsh do not each bind to SQLite or libcurl — they all compile
calls down to the same `CALL_BUILTIN` opcode (§3.3), resolved against one
shared registry. Adding a native capability is one C function plus one
registration call; no frontend changes at all.

The registration primitive (`builtin.c:2203`):

```c
void registerVmBuiltin(const char *name, VmBuiltinFn handler,
                       BuiltinRoutineType type, const char *display_name);
```

where `VmBuiltinFn` is `Value (*)(struct VM_s* vm, int arg_count, Value* args)`
— arguments arrive as a `Value` slice off the operand stack, and the returned
`Value` is pushed back (for `BUILTIN_TYPE_FUNCTION`) or discarded (for
`BUILTIN_TYPE_PROCEDURE`).

Registration does two things, and the second is what makes the frontends
inherit the extension:

1. **Runtime binding:** the canonicalized (lowercased) name → handler mapping
   is appended to the mutex-guarded `extra_vm_builtins` table and indexed for
   O(1)-ish lookup. Re-registering an existing name *replaces* the handler —
   deliberate, so a frontend or embedder can override a stock builtin.
2. **Compile-time visibility:** `registerBuiltinFunction(reg_name, declType,
   NULL)` synthesizes an `AST_FUNCTION_DECL`/`AST_PROCEDURE_DECL` for the
   builtin. Every frontend's semantic pass resolves identifiers against this
   same declaration table, which is exactly why `YyjsonRead(...)` is a valid
   expression in Pascal, Rea, CLike, Aether, and exsh the moment
   `registerYyjsonBuiltins()` has run — the compilers literally see it as a
   pre-declared function.

Names are case-insensitive at the call site (canonicalized on registration
and lookup; the Chapter 2 "builtin lowercase map" persists the pairing into
`.bc` files so the VM never re-lowercases at runtime).

**Extension categories.** Stock extensions live under `src/ext_builtins/`,
one directory per category, each gated by a CMake option and registered
exactly once per process via `pthread_once` (`register.c:25-60`):

| Category | Gate | Surface |
|----------|------|---------|
| `math`, `strings`, `system`, `user` | `ENABLE_EXT_BUILTIN_*` | scalar/string/OS helpers |
| `sqlite` | `ENABLE_EXT_BUILTIN_SQLITE` | full SQLite binding (§4.2) |
| `yyjson` | `ENABLE_EXT_BUILTIN_YYJSON` | JSON handles (§3.4) |
| `graphics`, `threed` | `ENABLE_EXT_BUILTIN_GRAPHICS` / `_3D` | SDL-backed drawing |
| `openai` | `ENABLE_EXT_BUILTIN_OPENAI` | `OpenAIChatCompletions` (§4.3) |
| `query` | always | introspection: programs can ask which categories/functions this binary was built with (`extBuiltinRegisterCategory`/`extBuiltinRegisterFunction` feed the queryable catalog) |

The query category closes the loop on optionality: since any category can be
compiled out, portable PSCAL code can probe for a capability before calling
it instead of dying on an unknown-builtin runtime error.

Because registration is data, not linkage, the same mechanism serves *host
applications embedding the VM*: register your own `VmBuiltinFn`s before
running a chunk and your domain API is a first-class function in all five
languages. This is the intended extension seam — not new opcodes (which
renumber the ISA, §3.0), and not `CALL_HOST` (reserved for the small set of
VM↔frontend-runtime hooks like closure creation).

### 4.0a Effect classes, the `--deny` sandbox, and record/replay (VM 2.0 Phase 6)

> Source of truth: `src/backend_ast/builtin.h`/`.c` (effect-mask storage and
> classification) and `src/vm/vm_fx_policy.{h,c}` (the CLI/env surface,
> the deny check, and the record/replay journal). Docs/pscal_vm2_plan.md §6.3
> has the full design history, including two real bugs this feature's own
> adversarial testing found and fixed.

`registerVmBuiltin()` (§4.0) now takes a fifth argument, `EffectMask
effect_mask` — a bitmask over `FX_PURE | FX_IO | FX_NET | FX_PROC | FX_CLOCK
| FX_RANDOM` (`core/effect_mask.h`). Every call site across `ext_builtins/*`
passes one explicitly; the ~500-entry core dispatch table (§4.0's static
`vmBuiltinDispatchTable[]`) is classified by name instead, against the same
130-name table `pscalBuiltinNameIsEffectful()` already used for the Aether
FX-001 gate — so that predicate's behavior didn't change, only its backing
data type (bool → mask). `getVmBuiltinEffectMaskById(id)` is the one lookup
that matters at runtime: O(1) after the first call (core-table masks are
computed once and cached; extension masks are stored directly at
registration time in a sparse per-id override array).

This mask is checked at exactly one place: right before
`CALL_BUILTIN`/`CALL_BUILTIN_PROC` would otherwise invoke the handler
(`vmApplyFxPolicy()` in `vm.c`). When no policy is configured
(`pscalFxPolicyActive()` false — no `--deny`, no `PSCAL_VM_DENY`, no active
record/replay journal), that's the entire cost: a few static-bool/int
comparisons, no mutex, no per-id lookup. This is what makes the feature a
true no-op for every program that doesn't opt in.

**Enforce (the sandbox).** `--deny io,net,proc,clock,random` (or `all`; also
settable via `PSCAL_VM_DENY`) makes any builtin whose mask intersects the
denied set raise a clean `runtimeError()` before the handler runs — the same
error path as any other runtime error, not a crash. This is the mechanism
that makes it safe to run untrusted or model-generated PSCAL programs
unattended at fleet scale (the aether_doc_bench / idea-miner harnesses'
motivating use case): `--deny net,proc` denies network egress and
process/thread spawning while leaving ordinary compute and stdout intact.

**Record/replay.** `--fx-record <path>` journals every *effectful*
builtin call's return value and VAR-parameter writebacks, in call order;
`--fx-replay <path>` substitutes the journal for execution on a later run,
so a flaky or hard-to-reproduce program (an HTTP-dependent eval, say) can be
replayed byte-identically without touching the network again. A
name/arg-count mismatch between the journal and the program's actual call
sequence aborts cleanly with a diagnostic — never a silent desync.

Not every effectful call is safely substitutable, and getting this wrong
corrupts state rather than merely producing a wrong answer — adversarial
testing found this the hard way (Docs/pscal_vm2_plan.md §6.3 has the full
account). The rule the implementation landed on: a call is journaled as
`substitutable` only if its result and every `VAR` writeback can be
faithfully captured.
- **File handles are not substitutable.** `assign`/`reset`/`rewrite`/`close`
  take their file variable by address (`TYPE_POINTER` to a `TYPE_FILE`
  Value) because the real handler must open/close an actual OS file
  descriptor in place; there's no meaningful way to "replay" that. These
  calls are marked non-substitutable at record time and always re-execute
  live on replay too (`PSCAL_FX_REPLAY_RUN_LIVE`) — after first verifying
  the journal's name/arg-count still match, so a genuine desync is still
  caught.
- **`MStream` out-params are substitutable.** `HttpRequest`'s `Contents`
  argument (and similar) arrives as a bare `TYPE_MEMORYSTREAM`, not a
  pointer — `MStream` is already reference-counted shared state, so passing
  it by ordinary value is sufficient for the callee to mutate what the
  caller sees. The journal captures its buffer bytes directly (kept
  NUL-terminated, matching `MStreamBuffer()`'s expectation that the buffer
  is a C string) and replays them into the caller's already-live `MStream`.

Net effect: a program mixing file I/O and HTTP requests replays with
byte-identical output — the file operations simply re-execute against the
same on-disk files (correct, since a live handle can't be faithfully
replayed), while the HTTP responses genuinely come from the journal, even
if the on-disk fixture backing a `file://` loopback request changed between
the record and replay runs. `Tests/vm_fx_policy/replay_http_file.p` is the
regression test for exactly this.

### 4.0b `dlopen` plugins: the same seam, out-of-process (VM 2.0 Phase 7)

> Source of truth: `backend_ast/pscal_ext_api.h` (the ABI), `backend_ast/
> pscal_ext_api.c` (the host-side vtable implementation), `ext_builtins/
> plugin_loader.{h,c}` (the loader and CLI/env wiring), and
> `components/pscal-core/plugins/sqlite/sqlite_ext_plugin.c` (the proof).
> Docs/pscal_vm2_plan.md §7.1 has the full design history, including the
> fork/recursive-mutex hazard this feature's own adversarial testing found.

§4.0 covers builtins registered *inside* the host binary at compile/link
time. Phase 7 adds a second path to the same seam: a builtin category
compiled as a separate shared library (`.dylib`/`.so`) and loaded at
runtime via `dlopen`, registering through exactly the same
`registerVmBuiltin` mechanism underneath -- once loaded, a plugin's
builtins are indistinguishable from an in-tree category's to every
frontend's compiler (same `CALL_BUILTIN`/`CALL_BUILTIN_PROC` opcodes, same
compile-time declaration synthesis). What's different is the ABI boundary:
a plugin's translation unit does not, and must not, include any pscal-core
internal header. It includes exactly one:

```c
#include "backend_ast/pscal_ext_api.h"
```

That header defines `PscalExtHostApi`, a vtable of function pointers the
host hands to the plugin's single entry point:

```c
int pscal_ext_register(const PscalExtHostApi* host, uint32_t host_abi);
```

The vtable covers registration (`register_builtin`, mirroring
`registerVmBuiltin`'s real `EffectMask`-carrying signature, plus the
`extBuiltinRegister*` introspection-catalog calls so a plugin's functions
show up in `--dump-ext-builtins`), diagnostics (`runtime_error`), a curated
set of Value constructors/accessors (`make_int`/`make_string`/`as_int64`/
`as_cstring`/etc., plus `is_string_type`/`is_intlike_type`/`is_real_type`
predicates over `VarType`), and a generic, reusable handle table
(`PscalExtHandleTable`) -- a realloc-growable, mutex-guarded tagged-slot
array, the same shape `sqlite_builtins.c` and `yyjson_builtins.c` each
hand-roll privately today (neither was touched; the generic helper is for
plugins). `Value`/`VarType` are the only pscal-core types a plugin ever
sees, and only by value/opaque discriminant -- the `AS_*`/`PSCAL_VALUE_PTR`
macros that dereference boxed heap wrappers (§1.2, Chapter 3) are
deliberately absent from the plugin-facing surface; a plugin reads a
`Value`'s scalar payload only through the vtable's accessor functions.

**Loading.** `--ext <path>` (validated eagerly -- the file must exist and
be readable -- matching `--fx-record`/`--fx-replay`'s own early-open-failure
convention, §4.0a) and the `PSCAL_EXT_DIR` environment variable (scanned
for platform shared libraries, each treated as if passed via `--ext`) are
recognized by all five frontends' CLI parsing (pascal/rea/clike/exsh
directly; aether for free, since its executable compiles rea's `main.c`
under a different `PSCAL_FRONTEND_MAIN_NAME`), via the same
`pscalFxIsCliFlag`-style shared-parsing pattern §4.0a's `--deny`/
`--fx-record`/`--fx-replay` already established
(`pscalExtIsCliFlag`/`pscalExtHandleCliFlag`, `ext_builtins/
plugin_loader.h`). The actual `dlopen` happens later, from
`pscalExtLoadRegisteredPlugins()`, called at the end of
`registerExtendedBuiltinsOnce()` -- the same `pthread_once`-guarded,
exactly-once seam §4.0's static in-tree categories register from, and
necessarily *after* `initSymbolSystem()` has run (a plugin's compile-time
declaration synthesis needs the global symbol/procedure hash tables to
already exist).

**A malformed or hostile plugin must never crash the host.** Every load
goes through a two-phase sequence: first, a crash-isolated dry-run probe in
a `fork()`ed child (dlopen, dlsym, call the entry point against a host API
whose registration-related entries are inert no-op stubs -- see the
callout below for why); only if the child reports success (exit code 0,
not killed by a signal) does the parent perform the real, in-process
`dlopen` + entry-point call that actually registers the plugin's builtins.
An ABI major-version mismatch is the plugin's own responsibility to detect
(`PSCAL_EXT_ABI_MAJOR_OF(host_abi)` against the major it was built against)
and reject by returning nonzero -- the loader has no way to know a
plugin's expected version ahead of the call, so it treats "entry point
returned nonzero" uniformly, whether the cause was an ABI mismatch or any
other plugin-detected startup failure.

> **Why the probe host stubs out registration but not everything else.**
> `pscalExtLoadRegisteredPlugins()` runs nested inside
> `populateBuiltinRegistry()`'s already-held recursive `builtin_registry_mutex`
> (`backend_ast/builtin.c`). Forking while holding a recursive mutex is
> unsafe: the mutex's owner bookkeeping is thread/process-identity based
> and doesn't survive `fork()`, so a child that re-enters it (as
> `register_builtin -> registerVmBuiltin -> registerBuiltinFunction` does)
> deadlocks forever on a lock whose recorded owner no longer resolves in
> the child process -- found empirically via a hung smoke test, not by
> inspection (`sample` showed the child stuck in
> `_pthread_mutex_firstfit_lock_wait`). The fix gives the probe child a
> *mixed* host API: `register_builtin`/`register_category`/
> `register_group`/`register_function_entry`/`runtime_error` are no-op
> stubs (the only five that touch that shared, lock-protected state);
> every other vtable entry -- Value construction/accessors/predicates, the
> handle-table family -- is the real implementation, since those touch no
> shared global state and are safe to exercise for real even inside a
> forked, throwaway child. This isn't just belt-and-suspenders: an earlier,
> all-stub probe host made `handle_table_create()` always return `NULL`,
> which made a well-behaved plugin that checks that return value (as the
> sqlite-as-plugin proof does) spuriously fail the probe on every load,
> even though the real, unforked load would have succeeded.

**Static categories keep static registration.** No in-tree category
(`ext_builtins/*`) was migrated to a plugin, and none is expected to be --
`dlopen` is an additional loading mechanism for out-of-tree/embedder
extensions, not a replacement for what already ships. The one exception
proving the mechanism is a *second*, separately-built copy of the sqlite
category (`components/pscal-core/plugins/sqlite/sqlite_ext_plugin.c`),
implemented using only `pscal_ext_api.h`, compiled to
`sqlite_ext_plugin.{dylib,so}`, and loadable via `--ext` -- it registers
the identical 20 `sqlite*` builtins the static category does and produces
byte-identical output (`Tests/vm_ext_plugin/sqlite_plugin_smoke.pas`,
wired into `Tests/run_all_suites.py --include-vm-ext-plugin`); the static
`ext_builtins/sqlite/sqlite_builtins.c` is untouched and is what ships by
default.

**iOS.** No `dlopen` culture, static linking only. `plugin_loader.c`'s real
loading path is compiled out entirely under `PSCAL_TARGET_IOS` (mirroring
`ext_builtins/register.c`'s existing `registerShellFrontendBuiltins()`
special-casing); `--ext`/`PSCAL_EXT_DIR` are still recognized there so a
shared script doesn't hit "unknown option", but produce an immediate,
clear "not supported on this platform" error rather than silently
compiling in a nonfunctional path.

### 4.1 The Network Operations Engine

#### Session model

HTTP state lives in a fixed pool of 32 slots (`MAX_HTTP_SESSIONS`), each
wrapping a libcurl easy handle. As with JSON and SQLite handles, the PSCAL
program holds only an integer index:

```c
typedef struct HttpSession_s {
    CURL* curl;
    struct curl_slist* headers;   // accumulated request headers
    struct curl_slist* resolve;   // host:port:address pinning entries
    long timeout_ms;              // default 15000
    long follow_redirects;        // default 1
    char* user_agent;             // default "PscalInterpreter/1.0"
    long last_status;
    // TLS / proxy
    char* ca_path;                // CURLOPT_CAINFO
    char* client_cert, *client_key;  // mutual TLS
    char* proxy, *proxy_userpwd;  long proxy_type;  // http/https/socks4/socks5
    long verify_peer, verify_host;   // default 1/1 (verify_host maps 1 -> CURL's 2)
    long force_http2, alpn;
    long tls_min, tls_max;        // 10/11/12/13 -> TLS 1.0..1.3
    char* ciphers;                // CURLOPT_SSL_CIPHER_LIST
    char* pinned_pubkey;          // CURLOPT_PINNEDPUBLICKEY
    // Behavior
    char* accept_encoding;  char* cookie_file, *cookie_jar;
    long max_retries;  long retry_delay_ms;        // exponential-backoff retry
    curl_off_t max_recv_speed, max_send_speed;     // rate limiting
    char* upload_file;  char* basic_auth;
    // Last-result state
    char* last_headers;           // raw response headers, accumulated by callback
    int   last_error_code;  char* last_error_msg;
} HttpSession;
```

Security defaults are correct-by-default: peer and host verification are ON
at allocation; a program must explicitly opt out via `HttpSetOption`. TLS
flexibility runs the full practical range — CA override, mutual TLS
(cert+key), version pinning (`tls_min`/`tls_max`), cipher-list control, and
public-key pinning for the paranoid tier.

#### Session lifecycle

```mermaid
stateDiagram-v2
    [*] --> Allocated : HttpSession() -> id (slot from pool of 32)
    Allocated --> Configured : HttpSetOption / HttpSetHeader (repeatable)
    Configured --> Configured : more options / headers
    Configured --> InFlight : HttpRequest / HttpRequestToFile (sync, blocks thread)
    Configured --> AsyncPending : HttpRequestAsync -> task
    InFlight --> Completed : status stored in last_status,\nheaders in last_headers
    Completed --> Configured : session reusable, options persist
    AsyncPending --> Completed : HttpAwait(task, out) / HttpTryAwait
    AsyncPending --> Cancelled : HttpCancel(task)
    Completed --> [*] : HttpClose(id) -> curl_easy_cleanup, slot freed
```

The sync path (`vmBuiltinHttpRequest`) configures the easy handle from
session state on every call — URL, write callback into a PSCAL `MStream`
(or a `DualSink` teeing to both a `FILE*` and the stream for
request-to-file-plus-buffer), header accumulator (`headerAccumCallback`
reallocs `last_headers` as chunks arrive), timeout, redirects, and the whole
TLS block — then runs `curl_easy_perform` on the *calling* interpreter
thread. Post-request, the program inspects `HttpGetLastHeaders`,
`HttpGetHeader(name)`, and `HttpErrorCode`.

#### The async layer

**VM 2.0 Phase 5a checkpoint 5a-iii** retired the async layer's own 32-slot
job pool (`MAX_HTTP_ASYNC`/`g_http_async[]`/`g_http_async_mutex`/
`httpAllocAsync` — deleted outright, no deprecation wrapper) in favor of
riding the VM's general-purpose task machinery (§1.4a/§1.4b): every async
request is now a `TYPE_TASK` value, heap-allocating its own `HttpAsyncJob`
and spawning through `vmTaskCreateNative` instead of a raw `pthread_create`
into a fixed-size array:

```c
typedef struct HttpAsyncJob_s {
    int session;                 // originating session id
    char *method, *url, *body;  size_t body_len;
    MStream* result;             // allocated by the job thread
    long status;  char* error;
    /* ...a full mirror copy of every session option at submission time:
       TLS block, proxy block, retries, rate limits, cookie jars,
       plus deep-copied curl_slist headers/resolve entries... */
    long long dl_now, dl_total;  // live progress counters, published via
                                  // vmTaskReportProgress/vmTaskGetProgress
    VM* threadVm;                 // set at httpAsyncWork's entry so curl's
                                   // progress callbacks (which only receive
                                   // this job as clientp) can still reach
                                   // vmTaskReportProgress and the owning
                                   // Thread's cancelRequested flag
} HttpAsyncJob;
```

The **mirror copy is still the design point**: `HttpRequestAsync` snapshots
the session's entire configuration into the job before the worker starts, so
the program can immediately reconfigure or reuse the session — even fire
another async request from it — without racing the in-flight job. Session
and job share nothing after submission except the session id used to write
back `last_status`/`last_headers` on completion (now done inside a single
`httpAsyncFinish` helper called from every one of the worker's exit paths,
success or failure, rather than scattered across `HttpAwait`'s old
post-`pthread_join` code).

The waiting API is the same small state machine as before, just over a task
handle instead of a bare job-array index:

- `HttpIsDone(task)` — non-blocking poll, `vmTaskIsDone`.
- `HttpTryAwait(task, out)` — non-blocking claim: returns the result only if
  finished, via `vmThreadTakeResult`.
- `HttpAwait(task, out)` — blocks until done (`vmThreadTakeResult`), then
  copies the job's result buffer into the caller-supplied `MStream`.
- `HttpCancel(task)` — `vmThreadCancel` sets the owning `Thread`'s
  `cancelRequested` atomic; the job's curl progress callbacks and its
  `file://` fast-path loop poll it directly at their own natural safe
  points (no job-local cancel flag — see §1.4b for why an earlier draft's
  per-job hook was removed after a confirmed race).
- `HttpGetAsyncProgress`/`HttpGetAsyncTotal` — `vmTaskGetProgress`, backed by
  the owning `Thread`'s `nativeProgressNow`/`nativeProgressTotal`.

CLike callers declare the handle with the language's new `task` type
(`task id = HttpRequestAsync(...)`); Pascal's existing `id: integer`-declared
call sites are unaffected (`ast.c`'s shared builtin-return-type inference for
these two builtins deliberately still reports `TYPE_INTEGER`, so Pascal's
loose assignment codegen keeps passing the real `TYPE_TASK` runtime value
through untouched — see `Docs/pscal_vm2_plan.md` §6.1's 5a-iii writeup for
why forcing `TYPE_TASK` there instead broke those fixtures).

#### Sequence: a thread dispatching an async TLS request

```mermaid
sequenceDiagram
    participant BC as Interpreter thread<br/>(bytecode)
    participant REG as Builtin registry
    participant SES as HttpSession[i]<br/>(slot pool)
    participant POOL as VM thread pool<br/>(threads[], §1.4/§1.4b)
    participant JOB as HttpAsyncJob<br/>(heap-allocated)
    participant NET as libcurl / TLS / network

    BC->>REG: CALL_BUILTIN 'httpsession'
    REG->>SES: httpAllocSession() — verify_peer=1, verify_host=1, 15s timeout
    SES-->>BC: push session id i
    BC->>SES: CALL_BUILTIN 'httpsetoption'(i,'tls_min','13'), 'httpsetheader'(...)
    BC->>REG: CALL_BUILTIN 'httprequestasync'(i,'GET',url)
    REG->>JOB: calloc HttpAsyncJob, MIRROR-COPY all session options,<br/>deep-copy header slists
    REG->>POOL: vmTaskCreateNative(httpAsyncWork, job, httpAsyncJobFree)
    POOL-->>BC: push TYPE_TASK value (returns immediately)
    par worker thread (a pool slot, not a dedicated job thread)
        JOB->>NET: curl_easy_perform — TLS handshake per mirrored options<br/>(CAINFO, VERIFYPEER, TLSVERSION, pinned key)
        NET-->>JOB: chunks -> result MStream, headers -> last_headers,<br/>progress -> vmTaskReportProgress
        JOB->>POOL: httpAsyncFinish -> vmThreadStoreResult({status, body})
    and interpreter thread continues
        BC->>BC: ...other bytecode; optionally CALL_BUILTIN 'httpisdone'(task)
    end
    BC->>POOL: CALL_BUILTIN 'httpawait'(task, out_mstream)
    POOL->>BC: vmThreadTakeResult; extract status/body into out;<br/>write status/headers back to session i; httpAsyncJobFree runs
    BC->>SES: CALL_BUILTIN 'httpclose'(i)
```

Note the concurrency layering: since checkpoint 5a-iii, async HTTP work runs
on the *same* growable pool `THREAD_CREATE`/`TaskSpawn` use (§1.4/§1.4b), not
a dedicated per-job pthread pool — a native `VMThreadCallback` (`httpAsyncWork`
here) occupies a pool slot for the duration of the transfer exactly like a
bytecode task does. A PSCAL program can have up to `VM_MAX_THREADS`
(env-overridable, default ceiling 4096 as of 5a-ii) tasks in flight across
*all* kinds of work — VM threads, `TaskSpawn`, and HTTP async alike — sharing
one pool, one ceiling, and one growth path, without any interpreter-level
blocking.

#### Sockets and DNS

Below the HTTP engine sits a raw socket layer (`SocketInfo_s`,
`builtin_network_api.c:46`) with the classic BSD surface as builtins:
`SocketCreate/Close/Connect/Bind/Listen/Accept/Send/Receive`,
`SocketSetBlocking`, `SocketPoll`, and `DnsLookup` — again integer handles,
again available identically from every frontend. HTTP is a convenience tier,
not a wall: servers and custom protocols are written against this layer.

### 4.2 Data & Storage Runtimes

Every native data subsystem follows the same handle discipline established
in §3.4: an integer on the PSCAL stack, a mutex-guarded native-side table
entry behind it, explicit lifecycle builtins, and a `runtimeError` (not
undefined behavior) on a stale or wrong-kind handle.

#### SQLite

The binding (`sqlite_builtins.c`, gated by `ENABLE_EXT_BUILTIN_SQLITE`) is a
faithful projection of the sqlite3 C API onto handles. Its table
distinguishes two handle kinds — connections and prepared statements — and
`SqliteClose` walks the table to finalize any statements still open against
the closing database (preventing the classic sqlite3 "unfinalized statement"
leak from being expressible):

```c
typedef struct {
    SqliteHandleKind kind;       // db or statement
    sqlite3 *db;                 // owner connection
    sqlite3_stmt *stmt;          // statement handles only
} SqliteHandleEntry;
static SqliteHandleEntry *sqliteHandleTable;   // grows on demand
```

The registered surface, grouped as the registration code groups it:

| Group | Builtins |
|-------|----------|
| connection | `SqliteOpen(path) -> db`, `SqliteClose(db)`, `SqliteExec(db, sql)`, `SqliteErrMsg(db)`, `SqliteLastInsertRowId(db)`, `SqliteChanges(db)` |
| statement | `SqlitePrepare(db, sql) -> stmt`, `SqliteStep(stmt)`, `SqliteReset(stmt)`, `SqliteFinalize(stmt)`, `SqliteClearBindings(stmt)` |
| binding | `SqliteBindText/Int/Double/Null(stmt, idx, v)` |
| metadata | `SqliteColumnCount/Type/Name(stmt, col)` |
| results | `SqliteColumnInt/Double/Text(stmt, col)` |

which supports the full prepared-statement loop from any frontend:

```pascal
db := SqliteOpen('bench.db');
stmt := SqlitePrepare(db, 'SELECT name, score FROM runs WHERE score > ?');
SqliteBindInt(stmt, 1, 25);
while SqliteStep(stmt) = 100 do   { SQLITE_ROW }
  writeln(SqliteColumnText(stmt, 0), ': ', SqliteColumnInt(stmt, 1));
SqliteFinalize(stmt);
SqliteClose(db);
```

#### JSON (yyjson)

Documented in §3.4; architecturally it is this same pattern with a
refcounting twist (VAL handles pin their parent DOC).

#### AI integration (OpenAI-compatible chat)

`OpenAIChatCompletions` (`openai_chat.c`, gated by
`ENABLE_EXT_BUILTIN_OPENAI`) rides the HTTP engine internally: it builds a
chat-completions JSON body, POSTs to an OpenAI-compatible endpoint (2–5
arguments: prompt/model plus optional endpoint and API key, with
environment-variable fallback for the key), and returns the response for the
program to pick apart with the yyjson builtins. It is a working demonstration
of the whole chapter's thesis: an "AI integration" required zero VM changes —
it is an ordinary registered builtin composed from two other builtin
subsystems, and the day it was registered it became callable from all five
languages.

### 4.3 Summary: the Composition Rules

The subsystem layer obeys four rules, uniformly:

1. **Handles are integers**; native state lives in mutex-guarded tables
   (sessions: fixed 32; async jobs: fixed 32; sqlite/json: growable).
   Nothing native ever crosses the operand stack.
2. **Lifecycles are explicit** (`*Close`/`*Free`/`*Finalize`); the VM does
   not garbage-collect native resources, but destructors are defensive
   (closing a DB finalizes its statements; freeing a session releases its
   curl handle and slists).
3. **Blocking is a choice**: every subsystem's blocking call runs on the
   calling interpreter thread; concurrency comes from either VM threads
   (§1.4) or subsystem-owned native threads (HTTP async), never from hidden
   yielding inside the interpreter.
4. **Everything enters through `registerVmBuiltin`**, which is why every
   capability in this chapter — and any capability an embedder adds tomorrow
   — is uniformly present in Pascal, Rea, CLike, Aether, and exsh.
