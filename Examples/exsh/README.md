# `exsh` Example Scripts

The exsh front end ships with a few minimal scripts that are used by the
regression suite and serve as reference material for new programs. Each script
lives alongside this README so you can run it directly with the `exsh`
executable (for example, `build/bin/exsh Examples/exsh/pipeline`).

## `pipeline`

```sh
#!/usr/bin/env exsh
echo "pipeline:start"
echo "alpha" | cat
echo "pipeline:end"
```

This script demonstrates how pipelines are lowered into the VM. `exsh` wires the
standard POSIX `|` operator through the builtin pipeline helpers so you can
combine multiple commands. Additional stages can be appended with more `|`
operators; each stage runs sequentially within the VM process.

## `conditionals`

```sh
#!/usr/bin/env exsh
echo "conditionals:start"
if true; then
    echo "then-branch"
else
    echo "else-branch"
fi
echo "conditionals:end"
```

High-level control flow such as `if`/`else` is parsed and executed inside the
VM. Because the current implementation executes helpers sequentially, this
example uses constant `true`/`false` commands to keep behaviour deterministic.
Replace them with your own conditions to drive different branches.

## `functions`

```sh
#!/usr/bin/env exsh
echo "functions:start"
export GREETING=hello
printenv GREETING
false
printenv EXSH_LAST_STATUS
true
printenv EXSH_LAST_STATUS
unset GREETING
printenv GREETING
echo "functions:end"
```

`exsh` scripts have access to the shared builtin catalog. The `export`/`unset`
pair mutates the host environment, and `EXSH_LAST_STATUS` reflects the
result of the most recently executed command or pipeline. You can call any other
PSCAL builtins (HTTP, JSON, SQLite, etc.) from the same script to orchestrate
complex workflows.

## `builtins`

```sh
#!/usr/bin/env exsh
builtin IntToStr int:42
builtin Length str:exsh-demo
builtin getEnv str:HOME
builtin ParamCount
builtin atoi str:1337
builtin getpid
```

The `builtin` command exposes the VM's core and extended builtin catalog from
the shell. Arguments are parsed as strings by default; add an explicit prefix to
force other types:

- `str:<value>` – keep the payload as a string.
- `int:<value>` – parse the payload with `strtoll` (base `0`, so `0x`, `0` and
  decimal forms are accepted).
- `float:<value>` / `double:<value>` / `real:<value>` – parse the payload with
  `strtod`.
- `bool:<value>` / `boolean:<value>` – coerce common truthy/falsy literals.
- `nil` or `nil:` – pass the VM's `nil` value.

When a builtin returns a result the command prints it to `stdout`. Procedures
that return `void` simply update `EXSH_LAST_STATUS` to `0` on success so
you can chain them in conditionals.

## `threading_demo`

```sh
#!/usr/bin/env exsh
dns_thread=$(builtin ThreadSpawnBuiltin str:dnslookup str:localhost)
delay_thread=$(builtin ThreadSpawnBuiltin str:delay int:25)
WaitForThread "$delay_thread"
WaitForThread "$dns_thread"
dns_result=$(builtin ThreadGetResult "$dns_thread")
dns_status=$(builtin ThreadGetStatus "$dns_thread" bool:true)
printf "dns:%s (status:%s)\n" "${dns_result:-<empty>}" "$dns_status"
```

The thread helpers run allow-listed VM builtins on worker threads without
needing the legacy `ThreadDemo*` helpers. `ThreadSpawnBuiltin` accepts either a
builtin name or numeric id followed by its arguments, returning a thread handle.
`WaitForThread` joins the worker and reflects the stored success flag in
`EXSH_LAST_STATUS`, while `ThreadGetResult`/`ThreadGetStatus` retrieve the
worker's return value and success bit. The sample script spawns a DNS lookup and
an asynchronous delay, waits for both handles, and prints the resolved IP and
status flag.

## `threading_showcase`

```sh
#!/usr/bin/env exsh
tid=$(builtin ThreadSpawnBuiltin str:dnslookup "str:$host")
builtin ThreadSetName "$tid" "str:dns:$host"
WaitForThread "$tid"
result=$(builtin ThreadGetResult "$tid" bool:true)
stats=$(builtin ThreadStats)
```

`threading_showcase` expands on the basic demo by exercising every worker-pool
builtin in one run. It spawns multiple DNS lookups, queues a delay via
`ThreadPoolSubmit`, assigns human-readable names with `ThreadSetName`, and uses
`ThreadLookup` to show how the names map back to thread handles. After the
workers finish, the script demonstrates both result-collection styles (`get`
followed by `status` and the one-shot `get(..., true)`) before dumping the pool
snapshot provided by `ThreadStats`. Each worker now prints a
`threading_showcase:result:` line describing the join status, cached success
flag, and collected payload so the transcript records the full lifecycle for
every thread before the cached metadata is cleared for reuse. Set
`THREAD_SHOWCASE_DELAY_MS=<millis>` to adjust the queued delay.

## `sierpinski`

```sh
#!/usr/bin/env exsh
builtin ClrScr
builtin HideCursor
# ... see file for the full sequential renderer ...
```

The sequential variant renders the same fractal without relying on worker
threads. It shares the threaded demo's helpers for discovering the terminal
size, hiding the cursor, and recursively drawing the triangle, but runs all of
the recursion inline so it works on builds that omit the extended worker pool.
You can customise the recursion depth and drawing character via
`SIERPINSKI_LEVEL` and `SIERPINSKI_CHAR`:

```sh
SIERPINSKI_LEVEL=9 SIERPINSKI_CHAR="#" build/bin/exsh \
    Examples/exsh/sierpinski
```

## `sierpinski_threads`

```sh
#!/usr/bin/env exsh
builtin ClrScr
builtin HideCursor
# ... see file for the full threaded renderer ...
```

This port of the Pascal `SierpinskiTriangleThreads` demo renders the fractal
with three VM threads started via the `SierpinskiSpawnWorker` builtin. The
script queries the terminal size, hides the cursor, and dispatches three
recursive workers that call `GotoXY`/`Write` to fill the screen. Each thread id
is joined with the standard `WaitForThread` builtin so the rendering stays
inside a single exsh process. By default it uses level 13 detail; set
`SIERPINSKI_LEVEL` (and optionally `SIERPINSKI_CHAR`) before invocation to tweak
the recursion depth and drawing character:

```sh
SIERPINSKI_LEVEL=9 SIERPINSKI_CHAR="#" build/bin/exsh \
    Examples/exsh/sierpinski_threads
```

## `logical`

```sh
#!/usr/bin/env exsh
echo "logical:start"
false && echo "and-skipped"
true && echo "and-ran"
true || echo "or-skipped"
false || echo "or-ran"
echo "logical:end"
```

`exsh` wires logical connectors (`&&`, `||`) to dedicated helpers so commands can
short-circuit based on the status of the previous stage. This sample shows which
branches execute when paired with the standard `true`/`false` utilities.

## `redirection`

```sh
#!/usr/bin/env exsh
echo "redirection:start"
echo "alpha" > tmp_exsh_redirection.txt
echo "beta" >> tmp_exsh_redirection.txt
cat < tmp_exsh_redirection.txt
rm -f tmp_exsh_redirection.txt
echo "redirection:end"
```

Redirections map to the VM's file descriptor helpers. The script writes to a
temporary file, appends an extra line, reads it back via input redirection, and
removes the temporary resource.

## `env`

```sh
#!/usr/bin/env exsh
echo "env:start"
setenv TEST_ENV alpha
python3 -c 'import os; print(os.getenv("TEST_ENV", "<UNSET>") or "<EMPTY>")'
setenv TEST_ENV beta
python3 -c 'import os; print(os.getenv("TEST_ENV", "<UNSET>") or "<EMPTY>")'
setenv TEST_ENV
python3 -c 'import os; print(os.getenv("TEST_ENV", "<UNSET>") or "<EMPTY>")'
unsetenv TEST_ENV
python3 -c 'import os; print(os.getenv("TEST_ENV", "<UNSET>") or "<EMPTY>")'
echo "env:end"
```

Environment management now mirrors standard shells: `setenv` updates or
prints variables, while `unsetenv` removes them. The helper Python invocations
read back the current value so the behaviour is visible in automated tests.

## Running the examples

Configure and build the project first:

```sh
cmake -S . -B build
cmake --build build
```

You can then run any script directly:

```sh
build/bin/exsh Examples/exsh/pipeline
```

Pass additional arguments after the script path to expose them to `$0`, `$1`,
and so on inside the VM.
