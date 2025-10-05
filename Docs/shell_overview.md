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
build/bin/exsh script.psh [arguments]
```

`exsh` accepts the same tracing and bytecode inspection flags as the other front
ends:

- `-v` prints the generated version string and latest git tag.
- `--dump-ast-json` writes the parsed program to stdout in JSON form.
- `--dump-bytecode` disassembles the compiled bytecode before execution.
- `--dump-bytecode-only` disassembles and exits without running the VM.
- `--dump-ext-builtins` lists available shell builtins.
- `--no-cache` forces recompilation even if a cached bytecode artefact exists.
- `--vm-trace-head=<N>` traces the first `N` VM instructions (the same toggle as
  the Pascal and CLike front ends).

Example scripts live under `Examples/exsh/` and cover pipelines, conditionals,
and environment-aware builtins.

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
- `PSCALSHELL_LAST_STATUS` mirrors the most recent exit status observed by the
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
`build/bin/exsh script.psh 1 2 3` exposes `1`, `2`, and `3` to the program.

Interactive sessions also support history expansion beyond `!!`. Numeric
designators like `!-2` and `!42`, prefix/substring searches (`!foo`, `!?bar?`),
and word designators (`!$`, `!*`, `!^`, `!:2`) work against the recorded
history before execution, providing the usual convenience shorthands. Regular
expression lookups (`!?/expr/?`) and substitution designators (`!:s/old/new/` or
`!:gs/old/new/`) are honoured as well, with replacement strings supporting tab
escapes via `\t`.

## Builtins and interaction with the VM

exsh reuses the shared builtin registry defined in `backend_ast/builtin.c` and
extends it with orchestration helpers implemented in
`backend_ast/shell.c`. The following categories are available out of the box:

- Shell control builtins (`cd`, `pwd`, `exit`, `alias`, `export`, `setenv`,
  `unset`, `unsetenv`).
- Pipeline helpers (`__shell_exec`, `__shell_pipeline`, `__shell_and`,
  `__shell_or`, `__shell_subshell`, `__shell_loop`, `__shell_if`).
- The complete PSCAL builtin catalog (HTTP, sockets, JSON, extended math/string
  groups, optional SQLite/SDL bindings) via `registerAllBuiltins()`.

The `builtin` shell command bridges these catalogs into scripts. Invoke it with
the VM builtin name followed by any arguments. Values are treated as strings by
default; prefix a token with `int:`, `float:`/`double:`/`real:`, `bool:` or
`str:` to coerce the argument to the corresponding VM type. Supplying `nil` (or
`nil:`) passes the VM's `nil` value directly. When the target builtin returns a
value, `builtin` prints it to `stdout`; procedures that return `void` simply set
`PSCALSHELL_LAST_STATUS` to `0` on success.

High-level control-flow syntax (`if`, `while`/`until`, and `for`) lowers to the
loop helpers above, which cooperate with real VM jump opcodes. The runtime
evaluates conditions using shell truthiness rules and only executes the branch
or loop body whose guard succeeds.

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
