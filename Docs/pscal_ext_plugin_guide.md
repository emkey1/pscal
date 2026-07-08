# Writing a PSCAL VM plugin

A practical, task-oriented guide: write a plugin, build it, load it, call
it from any frontend, and control whether it's even allowed to load.

This is the "how" doc. For the "why" -- the ABI's design rationale, the
fork-based crash isolation, the sqlite-as-plugin proof, and the full
gating design -- see [Docs/pscal_vm2_plan.md §7.1](pscal_vm2_plan.md) and
the VM manual's
[ch4 §4.0b](pscal_vm_manual/pscal_vm_manual_ch4.md). This guide assumes
none of that background; it just gets you from zero to a working plugin.

## What a plugin is

A plugin is an ordinary shared library (`.dylib` on macOS, `.so` on
Linux) that adds new builtin functions to the PSCAL VM at runtime, without
recompiling any of the five language frontends (Pascal, Rea, CLike,
Aether, exsh). Once loaded, a plugin's functions are ordinary callables in
all five languages simultaneously -- there is no per-frontend integration
step.

The entire contract between a plugin and the host VM is one header,
`backend_ast/pscal_ext_api.h`. A plugin includes only that file -- never
any other pscal-core internal header -- and exports exactly one function:

```c
int pscal_ext_register(const PscalExtHostApi* host, uint32_t host_abi);
```

`host` is a table of function pointers back into the VM (register a
builtin, construct/inspect `Value`s, report a runtime error, allocate
handles). `host_abi` is the host's ABI version; check
`PSCAL_EXT_ABI_MAJOR_OF(host_abi)` against `PSCAL_EXT_ABI_MAJOR` before
touching `host` at all, and return nonzero to reject a version you don't
support. Return `0` on success.

## The example

[`components/pscal-core/plugins/examples/hello_plugin.c`](../components/pscal-core/plugins/examples/hello_plugin.c)
is a complete, minimal, heavily-commented plugin -- copy it as a starting
point. It registers two functions:

- `PluginPing()` -- takes no arguments, returns the integer `42`. The
  simplest possible thing that proves loading worked at all.
- `PluginGreet(name)` -- takes one string argument, returns a greeting.
  Shows the three things every argument-handling builtin needs: check
  `arg_count`, check the argument's type before reading it
  (`host->is_string_type(args[0].type)` -- `.type` is always safe to read
  directly; never touch `.bits` yourself), then read it through a host
  accessor (`host->as_cstring(...)`).

Read the file itself for the line-by-line commentary; it's short enough
to be the actual documentation. The rest of this guide is about what to
do with it.

## Build it

No CMake needed -- a plugin is just a shared library:

```sh
cd components/pscal-core
cc -shared -fPIC -I src -o hello_plugin.dylib plugins/examples/hello_plugin.c   # macOS
cc -shared -fPIC -I src -o hello_plugin.so    plugins/examples/hello_plugin.c   # Linux
```

That's the entire build. If your plugin links a real native library (e.g.
SQLite), give it its own CMake `MODULE` target instead --
[`components/pscal-core/plugins/sqlite/sqlite_ext_plugin.c`](../components/pscal-core/plugins/sqlite/sqlite_ext_plugin.c)
plus its target in the top-level `CMakeLists.txt` is the reference for
that shape (handle tables, linking `libsqlite3`, `.dylib` suffix forced on
Apple). For anything with no external dependency, the bare `cc` command
above is the whole story.

## Load it

Two ways, both recognized identically by every frontend and by
`pscalvm`:

```sh
# One-off, explicit path (repeatable -- multiple --ext flags load multiple plugins)
pascal --ext ./hello_plugin.dylib --no-cache prog.pas

# Or drop it in a directory and point PSCAL_EXT_DIR at it -- every
# .dylib/.so in the directory loads the same way --ext would load it
PSCAL_EXT_DIR=./my_plugins pascal --no-cache prog.pas
```

Then call it like any other builtin, from any frontend:

```pascal
{ Pascal }
program HelloPlugin;
begin
  writeln(PluginPing());
  writeln(PluginGreet('Aether'));
end.
```

```
{ Aether }
fn main() -> Void {
  fx {
    println(PluginPing());
    println(PluginGreet("Aether"));
  }
  ret;
}
```

```
// Rea
int main() {
  writeln(PluginPing());
  writeln(PluginGreet("Rea"));
  return 0;
}
main();
```

```c
// CLike
int main() {
  printf("%d\n", PluginPing());
  printf("%s\n", PluginGreet("CLike"));
  return 0;
}
```

All four (plus exsh) produce:

```
42
Hello, <name>!
```

## Gate it

Loading a plugin means running arbitrary native code in-process, with
full privileges -- **this is not sandboxed by anything else in the VM.**
The `--deny io,net,proc,clock,random` effect sandbox governs what a
builtin's *declared* effect mask lets it do; nothing stops a plugin's own
C code from doing more than it declares. A plugin that registers itself
`PSCAL_EXT_FX_PURE` can still open a socket. Treat loading a plugin as
equivalent to running any other native code you didn't write yourself --
because that's exactly what it is.

Two independent controls exist for *whether a plugin loads at all* (not
for what it does once loaded -- there is no such control):

- **Per-run: `--deny ext`** (or `PSCAL_VM_DENY=ext`). Composes with the
  existing sandbox flags in one place: `--deny net,proc,ext` denies
  network egress, process spawning, *and* native plugin loading. Also
  covered by the `all` shorthand. Rejects `--ext`/`PSCAL_EXT_DIR` cleanly
  before any `dlopen` is attempted.
- **Per-build: `-DPSCAL_ENABLE_EXT_PLUGINS=OFF`** (CMake option, default
  `ON`). Compiles the real `dlopen` path out of the binary entirely -- no
  flag or environment variable in that build can re-enable it. Use this
  when you need a guarantee that holds even against a process that
  controls its own command line and environment (e.g. an autonomously
  operating agent).

```sh
pascal --deny ext --ext ./hello_plugin.dylib --no-cache prog.pas
# Error: dlopen plugin loading (--ext/PSCAL_EXT_DIR) denied by
# --deny/PSCAL_VM_DENY policy (ext). ...
```

## Adversarial cases, if you're testing your own loader integration

`Tests/vm_ext_plugin/run.sh` is the regression suite for the loader itself
-- it's a good reference for what "fails cleanly" looks like in practice:
a corrupt/truncated library, a valid library missing
`pscal_ext_register`, an ABI-major mismatch, and an entry point that
segfaults or calls `abort()` all produce a clean diagnostic and a normal
process exit, never a host crash.

## Where to look next

- [`hello_plugin.c`](../components/pscal-core/plugins/examples/hello_plugin.c) -- this guide's example, read it directly
- [`sqlite_ext_plugin.c`](../components/pscal-core/plugins/sqlite/sqlite_ext_plugin.c) -- a complete, realistic plugin with a native dependency and a handle table
- [`pscal_ext_api.h`](../components/pscal-core/src/backend_ast/pscal_ext_api.h) -- the full ABI, with design commentary on every field
- [VM manual ch4 §4.0b](pscal_vm_manual/pscal_vm_manual_ch4.md) -- the reference entry (loading mechanism, crash isolation, both gates)
- [`pscal_vm2_plan.md` §7.1](pscal_vm2_plan.md) -- the full design history, including the bugs found while building this
