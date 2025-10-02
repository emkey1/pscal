# Shell Front End (`psh`)

The shell front end compiles POSIX-style shell scripts to PSCAL bytecode and
executes them through the shared virtual machine. It focuses on orchestrating
external processes, pipelines, and the existing PSCAL builtin catalog instead of
implementing a full interactive shell.

## Command line usage

Build the project with CMake and invoke the new `psh` executable:

```sh
cmake -S . -B build
cmake --build build
build/bin/psh script.psh [arguments]
```

`psh` accepts the same tracing and bytecode inspection flags as the other front
ends:

- `-v` prints the generated version string and latest git tag.
- `--dump-ast-json` writes the parsed program to stdout in JSON form.
- `--dump-bytecode` disassembles the compiled bytecode before execution.
- `--dump-bytecode-only` disassembles and exits without running the VM.
- `--dump-ext-builtins` lists available shell builtins.
- `--no-cache` forces recompilation even if a cached bytecode artefact exists.
- `--vm-trace-head=<N>` traces the first `N` VM instructions (the same toggle as
  the Pascal and CLike front ends).

Example scripts live under `Examples/psh/` and cover pipelines, conditionals,
and environment-aware builtins.

## Bytecode caching

Compiled scripts are cached in `~/.pscal/bc_cache`. Cache entries are now keyed
by both the source path and the compiler identifier (`shell`), so the new front
end can coexist with Pascal, CLike and Rea bytecode without collisions. Use
`--no-cache` to force a recompile for the current script.

## Environment variables

The shell front end honours the standard process environment:

- Builtins such as `export`, `setenv`, and `unset` mutate the host environment
  for the current process. External utilities like `printenv` observe those changes
  immediately because `psh` runs everything in-process.
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
`build/bin/psh script.psh 1 2 3` exposes `1`, `2`, and `3` to the program.

Interactive sessions also support history expansion beyond `!!`. Numeric
designators like `!-2` and `!42`, prefix/substring searches (`!foo`, `!?bar?`),
and word designators (`!$`, `!*`, `!^`, `!:2`) work against the recorded
history before execution, providing the usual convenience shorthands.

## Builtins and interaction with the VM

`shell` reuses the shared builtin registry defined in `backend_ast/builtin.c` and
extends it with orchestration helpers implemented in
`backend_ast/shell.c`. The following categories are available out of the box:

- Shell control builtins (`cd`, `pwd`, `exit`, `alias`, `export`, `setenv`,
  `unset`, `unsetenv`).
- Pipeline helpers (`__shell_exec`, `__shell_pipeline`, `__shell_and`,
  `__shell_or`, `__shell_subshell`, `__shell_loop`, `__shell_if`).
- The complete PSCAL builtin catalog (HTTP, sockets, JSON, extended math/string
  groups, optional SQLite/SDL bindings) via `registerAllBuiltins()`.

High-level control-flow syntax (`if`, loops) is parsed and lowered to the
placeholder helpers above. Those helpers currently execute sequentially, so
scripts that rely on branching should gate behaviour using the exported status
variable or external utilities until proper VM jumps are wired in.

Because `psh` feeds every script through the same VM, shell programs can invoke
VM builtins directly—for example calling `HttpRequest` from a pipeline stage or
mixing shell conditionals with VM networking. Extended builtin groups that are
enabled at configure time are linked into the shell binary automatically and
exposed through the same dispatch table as other front ends.

## Working with examples and tests

Sample scripts demonstrating pipelines, conditionals, and environment-focused
builtins live in `Examples/psh/`. The regression suite under `Tests/shell/`
runs these scripts through `build/bin/psh`, checks stdout/stderr, and verifies
exit codes. The CI workflow executes the new suite alongside the Pascal, CLike,
and Rea test runs.
