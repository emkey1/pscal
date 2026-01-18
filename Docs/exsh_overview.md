# exsh Front End

The exsh front end compiles POSIX-style shell scripts to PSCAL bytecode and
executes them through the shared virtual machine. It focuses on orchestrating
external processes, pipelines, and the existing PSCAL builtin catalog instead of
implementing a full interactive shell.

## Command line usage

Build the project with CMake and invoke the new `exsh` executable:

```sh
cmake -S . -B build
cmake --build build
build/bin/exsh script.exsh [arguments]
```

`exsh` accepts the same tracing and bytecode inspection flags as the other front
ends:

- `-v` prints the generated version string and latest git tag.
- `-c <command> [name] [args...]` executes an inline command string. When
  additional arguments follow the string, the first argument becomes `$0` for
  diagnostics and the remainder populate the positional parameters so inline
  runs match Bash semantics.
- `--dump-ast-json` writes the parsed program to stdout in JSON form.
- `--dump-bytecode` disassembles the compiled bytecode before execution.
- `--dump-bytecode-only` disassembles and exits without running the VM.
- `--dump-ext-builtins` lists available shell builtins.
- `--no-cache` forces recompilation even if a cached bytecode artefact exists.
- `--semantic-warnings` enables semantic analysis diagnostics prior to execution.
- `--vm-trace-head=<N>` traces the first `N` VM instructions (the same toggle as
  the Pascal and CLike front ends).

Example scripts live under `Examples/exsh/` and cover pipelines, conditionals,
and environment-aware builtins.

Omitting the command string after `-c` emits the diagnostic `exsh: -c: option
requires an argument` and exits with status `2`, mirroring Bash's behaviour.

## Interactive mode

When `exsh` starts with a controlling TTY it first looks for `~/.exshrc`. If the
file exists it is executed with caching disabled; requesting `exit` inside the
file terminates the shell before the interactive loop begins.【F:src/shell/main.c†L178-L241】
Job control is initialised after the RC hook runs. The runtime only enables job
control when `exsh` owns the terminal; otherwise foreground/background helpers
degrade gracefully.【F:src/shell/main.c†L2413-L2420】【F:src/backend_ast/shell.c†L404-L485】【F:src/backend_ast/shell.c†L3729-L3731】

### Prompt customisation

`exsh` honours the `PS1` environment variable for its prompt. Bash-style escape
sequences are expanded, including time specifiers (`\t`, `\T`, `\@`, `\A`,
`\d`, `\D`), working-directory markers (`\w`, `\W`), user and host tokens
(`\u`, `\h`, `\H`), alert/escape characters, and octal/hex escapes. If `PS1`
is unset, the shell defaults to `exsh$ `.【F:src/shell/main.c†L354-L557】

### Line editing, search, and completion

Interactive sessions provide an Emacs-style editing experience. Common control
keys jump to the start/end of the line (`Ctrl-A`, `Ctrl-E`), move the cursor
(`Ctrl-B`, `Ctrl-F`), and navigate history (`Ctrl-P`, `Ctrl-N`). Kill and yank
commands (`Ctrl-U`, `Ctrl-K`, `Ctrl-W`, `Ctrl-Y`) manipulate the kill buffer,
while `Ctrl-L` repaints the screen. Incremental reverse search (`Ctrl-R`) and
`Alt-.` style last-argument insertion reuse the recorded history list. Tab
completion expands the current word via pathname globbing and prints ambiguous
matches in the familiar column layout.【F:src/shell/main.c†L1562-L1760】【F:src/shell/main.c†L2089-L2165】【F:src/shell/main.c†L1120-L1284】

### History handling

The interactive loop performs history expansion before executing a command and
echoes the expanded text when appropriate. All non-empty lines are recorded so
subsequent sessions can navigate, search, and reinsert previous commands. Tilde
expansion runs after history resolution so `!$` and friends can still be joined
with home-directory shortcuts.【F:src/shell/main.c†L2295-L2335】【F:src/backend_ast/shell.c†L3733-L3800】【F:src/backend_ast/shell.c†L3865-L3923】

History persistence is opt-in. Set `HISTFILE` (for example to
`$HOME/.exsh_history`) to load existing entries at startup and append each new
interactive line. `HISTSIZE` limits the in-memory list, and `HISTFILESIZE`
prunes the on-disk history to the last N entries (both default to 1000/2000
when unset). Writes use append mode to avoid clobbering other tabs; trimming
only rewrites the tail of the file when limits are exceeded.【F:src/shell/main.c†L3636-L3640】【F:src/backend_ast/shell/shell_word_expansion.inc†L196-L289】

## Bytecode caching

Compiled scripts are cached in `~/.pscal/bc_cache`. Cache entries are now keyed
by both the source path and the compiler identifier reported by exsh, so the front
end can coexist with Pascal, CLike and Rea bytecode without collisions. Use
`--no-cache` to force a recompile for the current script.

## Environment variables

The exsh front end honours the standard process environment:

- Builtins such as `export`, `setenv`, and `unset` mutate the host environment
  for the current process. External utilities like `printenv` observe those changes
  immediately because `exsh` runs everything in-process.
- `EXSH_LAST_STATUS` mirrors the most recent exit status observed by the
  runtime and is updated after every builtin or pipeline execution.
- Caching relies on `$HOME` to locate the cache directory and `$PATH` to resolve
  executables when necessary.

Direct parameter interpolation now behaves like a POSIX shell: `$NAME`,
`${NAME}`, `$?`, `$#`, `$@`, `$*`, and positional parameters expand to the
current environment and argument vector. `setenv` accepts `NAME [VALUE]` to set
variables (printing the environment when invoked without arguments), and the
new `unsetenv` builtin mirrors `unset` for scripts that prefer csh-style
naming. The VM argument vector (`$0`, `$1` …) maps directly to the parameters
passed after the script path (`gParamValues` inside the VM), so invoking
`build/bin/exsh script.exsh 1 2 3` exposes `1`, `2`, and `3` to the program.

Interactive sessions also support history expansion beyond `!!`. Numeric
designators like `!-2` and `!42`, prefix/substring searches (`!foo`, `!?bar?`),
and word designators (`!$`, `!*`, `!^`, `!:2`) work against the recorded
history before execution, providing the usual convenience shorthands. Regular
expression lookups (`!?/expr/?`) and substitution designators (`!:s/old/new/` or
`!:gs/old/new/`) are honoured as well, with replacement strings supporting tab
escapes via `\t`.

### Interactive startup configuration

When `exsh` starts in interactive mode it looks for `~/.exshrc` and sources the
file before presenting the prompt. The repository includes a ready-to-use
template under `etc/skel/.exshrc` that configures a colourful prompt, quality of
life aliases, PSCAL builtin helpers, and hooks for per-machine overrides. Copy
the file to your home directory (or use it as a reference when building your
own) to get a sensible default environment immediately.
## Builtins and interaction with the VM

exsh reuses the shared builtin registry defined in `backend_ast/builtin.c` and
extends it with orchestration helpers implemented in
`backend_ast/shell.c`. The following categories are available out of the box:

- Shell control builtins (`cd`, `pwd`, `exit`, `alias`, `export`, `setenv`,
  `unset`, `unsetenv`) and an interactive `help` builtin for listing and
  describing available commands. The directory-stack helpers (`dirs`, `pushd`,
  `popd`) mirror Bash: they maintain `$PWD`, print the stack after each
  mutation, and reject unsupported flag combinations so parity tests stay
  precise.【F:src/backend_ast/shell/shell_builtins.inc†L2495-L2627】【F:Tests/exsh/tests/bash_parity_directory_stack.exsh†L1-L32】
- Pipeline helpers (`__shell_exec`, `__shell_pipeline`, `__shell_and`,
  `__shell_or`, `__shell_subshell`, `__shell_loop`, `__shell_if`).
- The complete PSCAL builtin catalog (HTTP, sockets, JSON, extended math/string
  groups, optional SQLite/SDL bindings) via `registerAllBuiltins()`.
- Command-dispatch diagnostics such as `hash`, `enable`, and `which` let
  scripts inspect the cached PATH table or enumerate disabled builtins without
  leaving the shell.【F:src/backend_ast/shell/shell_builtins.inc†L7243-L7310】

The `builtin` shell command bridges these catalogs into scripts. Invoke it with
the VM builtin name followed by any arguments. Values are treated as strings by
default; prefix a token with `int:`, `float:`/`double:`/`real:`, `bool:` or
`str:` to coerce the argument to the corresponding VM type. Supplying `nil` (or
`nil:`) passes the VM's `nil` value directly. When the target builtin returns a
value, `builtin` prints it to `stdout`; procedures that return `void` simply set
`EXSH_LAST_STATUS` to `0` on success.

High-level control-flow syntax (`if`, `while`/`until`, and `for`) lowers to the
loop helpers above, which cooperate with real VM jump opcodes. The runtime
evaluates conditions using shell truthiness rules and only executes the branch
or loop body whose guard succeeds.

Job control follows Bash's conventions. `jobs` collates the active table,
`disown` removes entries without killing them, `fg`/`bg` resume stopped
pipelines, and `wait`/`kill` honour `%`-style specifiers while normalising
signal names and integers. All helpers consult the shared job ledger so output
stays stable when pipelines fork or launch nested processes.【F:src/backend_ast/shell/shell_builtins.inc†L7949-L8119】

## Accessing core and extended builtins

`shellRunSource` registers the full PSCAL builtin catalog before compiling a
script, so every shell run has access to the same VM services as the Pascal and
C-like front ends.【F:src/shell/runner.c†L53-L105】 Shell-specific helpers are
compiled into the runtime and exposed through builtin handlers such as `cd`,
`jobs`, `fg`, and `bg` for job control, along with `builtin` for invoking VM
routines directly.【F:src/backend_ast/shell.c†L5985-L6767】

The `builtin` command accepts the VM builtin name followed by arguments. Prefix
tokens with `int:`, `float:`/`double:`/`real:`, `bool:`, `str:`/`raw:`, or
`nil`/`nil:` to coerce shell words into the expected VM type; otherwise values
are passed as strings. Return values are printed automatically when the
underlying builtin reports success.【F:src/backend_ast/shell.c†L3462-L3561】【F:src/backend_ast/shell.c†L6767-L6805】

Extended builtin groups are linked at build time. You can inspect the compiled
inventory by running `exsh --dump-ext-builtins`, which produces the same
category/group/function listing used by other front ends.【F:src/shell/main.c†L2351-L2387】
Available categories include math, system, SQLite, graphics, yyjson, and other
optional modules controlled by the `-DENABLE_EXT_BUILTIN_*` CMake flags.【F:Docs/extended_builtins.md†L15-L44】

EXIT traps now run with a clean execution context. The runtime snapshots exit
and loop flags before invoking the trap body, merges any new requests after it
finishes, and only escalates deferred `set -e` failures once conditional guards
have had a chance to handle them. This keeps `&&`, `||`, and `if` short-circuits
behaving like Bash while preserving pending exits when the trap requests an
override.【F:src/backend_ast/shell/shell_runtime_state.inc†L1667-L1779】

## Worker threads and builtin orchestration

`ThreadSpawnBuiltin` lets shell scripts queue allow-listed VM builtins—such as
`dnslookup`, the asynchronous HTTP helpers, and `delay`—onto the shared worker
pool. Pass either the builtin name or numeric id followed by any arguments; the
call returns a thread handle. Pair it with `WaitForThread` to block until the
worker finishes and surface its status through `EXSH_LAST_STATUS`, or call
`ThreadGetResult(handle, true)` to read and immediately clear any cached
payload/status pair so the slot can be reused.【F:Examples/exsh/threading_demo†L1-L30】

`ThreadPoolSubmit` exposes the same interface but marks the call as
submit-only so interactive scripts can continue processing foreground commands.
Use `ThreadSetName` to attach descriptive labels to workers and
`ThreadLookup("label")` to map those names back to handles from other parts of
the script. `ThreadPause`, `ThreadResume`, and `ThreadCancel` expose cooperative
control over long-running assignments, while `ThreadStats` produces a JSON-ready
snapshot of pool usage for dashboards and logs.【F:src/backend_ast/builtin.c†L4930-L5794】

The bundled examples demonstrate these helpers in practice:

- `Examples/exsh/threading_demo` runs a DNS lookup alongside a timer and prints
  the stored result.
- `Examples/exsh/parallel-check` scales the pattern across an arbitrary host
  list, tagging each slot via `ThreadSetName` before reporting success and
  failure counts.

## Grammar coverage

The parser now accepts the broader POSIX grammar expected by automation-heavy
scripts. Here documents honour quoting rules (`<<word` expands variables while
`<<'word'` keeps literal text), brace groups run in the current shell context
and can appear inside pipelines, and leading `!` prefixes invert pipeline exit
statuses. Multi-line `for` loops with backslash continuations and complex
`case` clauses split across lines (including pattern alternation with `|`) are
all recognised. Targeted regression scripts under `Tests/exsh/tests/`
exercise these constructs so future grammar work can expand safely.

Because `exsh` feeds every script through the same VM, shell programs can invoke
VM builtins directly—for example calling `HttpRequest` from a pipeline stage or
mixing shell conditionals with VM networking. Extended builtin groups that are
enabled at configure time are linked into the exsh binary automatically and
exposed through the same dispatch table as other front ends.

## Working with examples and tests

Sample scripts demonstrating pipelines, conditionals, and environment-focused
builtins live in `Examples/exsh/`. The regression suite under `Tests/exsh/`
runs these scripts through `build/bin/exsh`, checks stdout/stderr, and verifies
exit codes. The CI workflow executes the new suite alongside the Pascal, CLike,
and Rea test runs.


## POSIX deviations (non-additive)

`exsh` intentionally omits or simplifies a handful of POSIX behaviours. These
differences are not additive extensions:

- `set` only recognises `-e`, `+e`, and `-o/+o errexit`; other options are
  ignored or flagged as errors.【F:src/backend_ast/shell.c†L6415-L6459】
- `trap` merely toggles a global flag—no handler strings are registered or
  executed, and non-string arguments fail validation.【F:src/backend_ast/shell.c†L6464-L6481】
- `local` does not introduce a new scope; it simply flips an internal marker and
  returns success.【F:src/backend_ast/shell.c†L6484-L6489】
- `read` supports `-p` prompts and `--` but otherwise rejects option flags, and
  assigns results by exporting environment variables (defaulting to `REPLY`)
  instead of populating shell-local variables.【F:src/backend_ast/shell.c†L6195-L6307】
