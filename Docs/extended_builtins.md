# Extending VM Built-ins

Pscal allows additional built‑in routines to be linked into the virtual
machine at build time.  This makes it easy to expose host functionality
without modifying the core source tree.  Optional built‑ins live under
`src/ext_builtins`, organised into categories that contain named groups of
related routines.  Groups may be organised hierarchically using `/`-delimited
paths (for example, `user/landscape/rendering`).  Intermediate groups are
created automatically when nested paths are registered, so extensions can
describe deep inventories without extra bookkeeping.

For a catalog of existing VM routines, see
[`pscal_vm_builtins.md`](pscal_vm_builtins.md).

## Available extended built-ins

The project currently ships several optional built‑in groups:

| Category | Location | Groups |
| -------- | -------- | ------ |
| **Math** | `src/ext_builtins/math` | `series` (`Factorial`, `Fibonacci`), `fractal` (`MandelbrotRow`), `constants` (`Chudnovsky`) |
| **System** | `src/ext_builtins/system` | `filesystem` (`FileExists`), `process` (`GetPid`), `timing` (`RealTimeClock`), `utility` (`Swap`), `shell` (`__shell_exec`, `__shell_pipeline`, `__shell_and`, `__shell_or`, `__shell_subshell`, `__shell_loop`, `__shell_if`, `cd`, `pwd`, `exit`, `export`, `unset`, `alias`) |
| **Strings** | `src/ext_builtins/strings` | `conversion` (`Atoi`) |
| **Yyjson** | `src/ext_builtins/yyjson` | `document` (`YyjsonRead`, `YyjsonReadFile`, `YyjsonDocFree`), `query` (`YyjsonFreeValue`, `YyjsonGetRoot`, `YyjsonGetKey`, `YyjsonGetIndex`, `YyjsonGetLength`, `YyjsonGetType`), `primitives` (`YyjsonGetString`, `YyjsonGetNumber`, `YyjsonGetInt`, `YyjsonGetBool`, `YyjsonIsNull`) |
| **Sqlite** | `src/ext_builtins/sqlite` | `connection` (`SqliteOpen`, `SqliteClose`, `SqliteExec`, `SqliteErrMsg`, `SqliteLastInsertRowId`, `SqliteChanges`), `statement` (`SqlitePrepare`, `SqliteFinalize`, `SqliteStep`, `SqliteReset`, `SqliteClearBindings`), `metadata` (`SqliteColumnCount`, `SqliteColumnType`, `SqliteColumnName`), `results` (`SqliteColumnInt`, `SqliteColumnDouble`, `SqliteColumnText`), `binding` (`SqliteBindText`, `SqliteBindInt`, `SqliteBindDouble`, `SqliteBindNull`) |
| **3D** | `src/ext_builtins/threed` | `physics` (`BouncingBalls3DStep`, `BouncingBalls3DStepUltra`, `BouncingBalls3DStepAdvanced`, `BouncingBalls3DStepUltraAdvanced`, `BouncingBalls3DAccelerate`), `rendering` (`BouncingBalls3DDrawUnitSphereFast`) |
| **Graphics** | `src/ext_builtins/graphics` | `window` (`InitGraph`, `CloseGraph`, `ClearDevice`, `UpdateScreen`, `GraphLoop`), `drawing` (`SetColor`, `DrawLine`, `FillRect`, `DrawCircle`, `GetPixelColor`), `textures` (`CreateTexture`, `LoadImageToTexture`, `RenderCopyEx`, `UpdateTexture`), `text` (`InitTextSystem`, `OutTextXY`, `RenderTextToTexture`), `input` (`PollKey`, `IsKeyDown`, `GetMouseState`, `WaitKeyEvent`), `audio` (`InitSoundSystem`, `LoadSound`, `PlaySound`, `StopAllSounds`, `IsSoundPlaying`), `opengl` (`GLBegin`, `GLRotatef`, `GLColor4f`, `GLIsHardwareAccelerated`) |
| **User** | `src/ext_builtins/user` | `landscape` → `landscape/configure` (`LandscapeConfigureProcedural`), `landscape/rendering` (`LandscapeDrawTerrain`, `LandscapeDrawWater`), `landscape/precompute` (`LandscapePrecomputeWorldCoords`, `LandscapePrecomputeWaterOffsets`, `LandscapeBuildHeightField`, `LandscapeBakeVertexData`) |
| **OpenAI** | `src/ext_builtins/openai` | `chat` (`OpenAIChatCompletions`) |

Individual categories can be enabled or disabled at configure time with
the following CMake options (all default to `ON`).  The 3D helpers follow the
SDL toggle: they default to `ON` when SDL support is requested and `OFF`
otherwise so headless builds avoid the extra surface area by default.

```
-DENABLE_EXT_BUILTIN_MATH=ON/OFF
-DENABLE_EXT_BUILTIN_STRINGS=ON/OFF
-DENABLE_EXT_BUILTIN_SYSTEM=ON/OFF
-DENABLE_EXT_BUILTIN_USER=ON/OFF
-DENABLE_EXT_BUILTIN_YYJSON=ON/OFF
-DENABLE_EXT_BUILTIN_SQLITE=ON/OFF
-DENABLE_EXT_BUILTIN_3D=ON/OFF
-DENABLE_EXT_BUILTIN_GRAPHICS=ON/OFF
-DENABLE_EXT_BUILTIN_OPENAI=ON/OFF
```

### Yyjson built-ins

The `yyjson` category wraps the bundled [yyjson](https://github.com/ibireme/yyjson) library and exposes helpers for parsing documents, walking objects and arrays, and converting primitive values. Each routine operates on integer handles returned by `YyjsonRead` or the various query helpers; release value handles with `YyjsonFreeValue` and dispose of documents with `YyjsonDocFree` when they are no longer needed.

### Sqlite built-ins

The `sqlite` category embeds the platform SQLite3 library and presents handle-based wrappers that work consistently across the Pascal, C-like, and Rea front ends. `SqliteOpen` returns a database handle for the supplied path (use `:memory:` for an in-memory database); invoke `SqliteClose` when finished to dispose of the connection. Use `SqliteExec` for simple statements or the `SqlitePrepare`/`SqliteStep`/`SqliteFinalize` trio for parameterised queries. Column accessors (`SqliteColumnInt`, `SqliteColumnDouble`, `SqliteColumnText`, etc.) operate on statement handles after `SqliteStep` yields a row. Binding helpers (`SqliteBindText`, `SqliteBindInt`, `SqliteBindDouble`, `SqliteBindNull`) populate positional parameters; call `SqliteReset` and `SqliteClearBindings` to reuse prepared statements. Diagnostic helpers such as `SqliteErrMsg`, `SqliteChanges`, and `SqliteLastInsertRowId` mirror their SQLite counterparts.

### OpenAI built-ins

The `openai` category exposes a small HTTP-based interface for calling the
`/chat/completions` endpoint. `OpenAIChatCompletions(model, messagesJson,
[optionsJson, apiKey, baseUrl])` sends a JSON payload to the service and
returns the raw response body. Provide a JSON array of message objects for the
conversation history; optional parameters let callers append additional request
fields, override the API key, or target a different endpoint. When the API key
argument is empty the runtime falls back to the `OPENAI_API_KEY` environment
variable. All three front ends ship helper libraries that build message arrays,
invoke the builtin, and extract the assistant's response text for quick
integration.

### exsh orchestration built-ins

The system category now enumerates a `shell` group that mirrors the runtime
helpers used by the standalone exsh front end. These routines are implemented
inside the VM, so scripts can orchestrate processes without spawning additional
interpreters.

- `__shell_exec(meta, ...)` launches a single command, honouring simple
  redirections encoded in its argument vector and optionally running in the
  background.
- `__shell_pipeline(meta)` initialises pipeline state so the subsequent
  `__shell_exec` calls connect their standard streams via POSIX pipes. Background
  pipelines are registered with the VM's job table for later polling.
- Logical helpers (`__shell_and`, `__shell_or`, `__shell_subshell`,
  `__shell_loop`, `__shell_if`) allow the compiler to model complex control
  structures without embedding host-specific branching.
- Classic shell utilities (`cd`, `pwd`, `exit`, `export`, `unset`, `alias`) run
  entirely inside the interpreter so callers avoid forking when they need to
  mutate the current environment.

Although primarily targeted by the exsh front end, these routines are exposed
to every VM client via the builtin registry and host function table.

## Discovering available categories at runtime

Programs can introspect the VM to see which extended built-in categories are
available and which routines each one provides.  All three front ends expose
the following helpers:

- `ExtBuiltinCategoryCount()` returns the number of registered categories.
- `ExtBuiltinCategoryName(index)` returns the name for a zero-based category
  index.
- `ExtBuiltinGroupCount(category)` reports how many groups belong to a category
  (the count includes the implicit `default` bucket when ungrouped routines are
  present).
- `ExtBuiltinGroupName(category, index)` retrieves a group's name.
- `ExtBuiltinGroupFunctionCount(category, group)` returns the number of routines
  within a group.
- `ExtBuiltinGroupFunctionName(category, group, index)` yields the name for a
  zero-based index inside a group.
- `ExtBuiltinFunctionCount(category)` reports how many functions belong to a
  category across all groups.
- `ExtBuiltinFunctionName(category, index)` returns the name for a zero-based
  function index within the given category.
- `HasExtBuiltin(category, function)` checks for a specific routine.

Pascal uses the Pascal-cased names shown above.  The C-like and Rea front ends
expose the same helpers with lowercase identifiers (for example,
`extbuiltincategorycount()`).

Complete runnable examples that exercise these helpers live under
`Examples/pascal/base/DumpExtBuiltins`,
`Examples/clike/base/DumpExtBuiltins`,
`Examples/rea/base/dump_ext_builtins`, and
`Examples/exsh/dump_ext_builtins`.

Example Pascal snippet that lists the available routines when the category is
present:

```pascal
var
  i, j, k: integer;
  cat, grp, fn: string;
begin
  for i := 0 to ExtBuiltinCategoryCount() - 1 do
  begin
    cat := ExtBuiltinCategoryName(i);
    writeln('Category: ', cat);
    for j := 0 to ExtBuiltinGroupCount(cat) - 1 do
    begin
      grp := ExtBuiltinGroupName(cat, j);
      writeln('  Group: ', grp);
      for k := 0 to ExtBuiltinGroupFunctionCount(cat, grp) - 1 do
      begin
        fn := ExtBuiltinGroupFunctionName(cat, grp, k);
        writeln('    ', fn);
      end;
    end;
  end;
end;
```

### Command-line discovery

When you need to inspect the VM without writing a program, each front end
accepts `--dump-ext-builtins` to emit the same inventory. The output uses a
line-oriented format that is easy for regression harnesses to parse:

```
$ pascal --dump-ext-builtins
category system
group system filesystem
function system filesystem FileExists
group system process
function system process GetPid
```

The `clike` and `rea` binaries expose the identical option and format, making
it straightforward to tailor front-end-specific test suites based on the
compiled VM's capabilities.


## Threading considerations

Extended built-ins execute inside the VM and may be called from multiple
threads when the host application uses the interpreter concurrently.  To
avoid race conditions or other undefined behavior:

- Avoid mutable global or static state, or guard it with appropriate
  synchronization primitives.
- Prefer thread-safe library routines and be cautious when sharing
  resources such as files or sockets.
- Keep critical sections short and do not hold locks while calling back
  into the VM.

The VM itself does not provide locking for custom built-ins, so each
extension is responsible for its own thread safety.

Front ends can now queue vetted helpers on worker threads via
`ThreadSpawnBuiltin` or `ThreadPoolSubmit`, collect results with
`ThreadGetResult`/`ThreadGetStatus`, and manage slots through `ThreadSetName`,
`ThreadLookup`, `ThreadPause`, `ThreadResume`, `ThreadCancel`, and
`ThreadStats`. Only the allow-listed routines enumerated in
`src/backend_ast/builtin.c` may execute this way; they are limited to re-entrant
HTTP/API helpers and `dnslookup`. Builtins that touch mutable global state must
stay off the list unless they provide their own synchronisation.

Consult [Docs/threading.md](threading.md) for pool sizing environment variables,
worker-naming rules, and advice on interpreting the metrics returned by
`ThreadStats` when you need to debug long-running builtins.

## Creating a new built-in

1. Choose a category under `src/ext_builtins` or create a new one.  For
   quick experiments, drop files into `src/ext_builtins/user`.
2. Drop a C file into that directory that defines the VM handler and a
   registration helper:

```c
static Value vmBuiltinFoo(struct VM_s* vm, int argc, Value* args) {
    /* ... */
}

void registerFooBuiltin(void) {
    registerBuiltinFunction("Foo", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("foo", vmBuiltinFoo);
}
```

3. Update the category’s `register.c` to call `registerFooBuiltin`.
4. Add the new source file to the category’s `CMakeLists.txt`.
5. Re‑run CMake and rebuild.  The routine is now available to both the
   Pascal and C‑like front ends.

### Example: System built-ins

The `system` category demonstrates two routines, `GetPid` and `Swap`.
`getpid.c` exposes the current process ID:

```c
#include <unistd.h>
#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinGetPid(struct VM_s* vm, int arg_count, Value* args) {
    (void)vm; (void)args;
    return arg_count == 0 ? makeInt(getpid()) : makeInt(-1);
}

void registerGetPidBuiltin(void) {
    registerBuiltinFunction("GetPid", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("getpid", vmBuiltinGetPid);
}
```

`realtimeclock.c` returns the current wall-clock time as a `DOUBLE`
measured in seconds since the Unix epoch. It uses the highest resolution
timer available on the host platform and reports monotonic results suitable
for simple timing and latency measurements:

```c
static Value vmBuiltinRealTimeClock(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "RealTimeClock expects no arguments.");
        return makeDouble(0.0);
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long double seconds = ts.tv_sec + ts.tv_nsec / 1000000000.0L;
    return makeDouble((double)seconds);
}

void registerRealTimeClockBuiltin(void) {
    registerVmBuiltin("realtimeclock", vmBuiltinRealTimeClock,
                      BUILTIN_TYPE_FUNCTION, "RealTimeClock");
}
```

`swap.c` accepts two `VAR` parameters and exchanges their contents:

```c
#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinSwap(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "Swap expects exactly 2 arguments.");
        return makeVoid();
    }
    if (args[0].type != TYPE_POINTER || args[1].type != TYPE_POINTER) {
        runtimeError(vm, "Arguments to Swap must be variables (VAR parameters).");
        return makeVoid();
    }
    Value* varA = (Value*)args[0].ptr_val;
    Value* varB = (Value*)args[1].ptr_val;
    if (!varA || !varB) {
        runtimeError(vm, "Swap received a NIL pointer for a VAR parameter.");
        return makeVoid();
    }
    if (varA->type != varB->type) {
        runtimeError(vm, "Cannot swap variables of different types (%s and %s).",
                     varTypeToString(varA->type), varTypeToString(varB->type));
        return makeVoid();
    }
    Value temp = *varA;
    *varA = *varB;
    *varB = temp;
    return makeVoid();
}

void registerSwapBuiltin(void) {
    registerBuiltinFunction("Swap", AST_PROCEDURE_DECL, NULL);
    registerVmBuiltin("swap", vmBuiltinSwap);
}
```

The category's `register.c` ties the helpers together:

```c
#include "backend_ast/builtin.h"

void registerFileExistsBuiltin(void);
void registerGetPidBuiltin(void);
void registerSwapBuiltin(void);

void registerSystemBuiltins(void) {
    registerFileExistsBuiltin();
    registerGetPidBuiltin();
    registerSwapBuiltin();
}
```

## Example usage

After rebuilding with the system built‑ins enabled, the following Pascal
program uses `GetPid` and `Swap`:

```pascal
program ShowBuiltins;
type
  PInt = ^integer;
var
  a, b: PInt;
begin
  writeln('PID = ', GetPid());
  New(a); New(b);
  a^ := 1; b^ := 2;
  Swap(a, b);
  writeln('After Swap: a=', a^, ' b=', b^);
  Dispose(a); Dispose(b);
end.
```

Running the program prints:

```sh
$ build/bin/pascal Examples/pascal/base/ShowExtendedBuiltins
PID = 12345
After Swap: a=2 b=1
```

The same built‑ins are available to the C‑like front end:

```c
int main() {
    printf("PID = %d\n", getpid());
    int* a;
    int* b;
    new(&a); new(&b);
    *a = 1; *b = 2;
    swap(&a, &b);
    printf("After Swap: a=%d b=%d\n", *a, *b);
    dispose(&a); dispose(&b);
    return 0;
}
```

```
$ build/bin/clike Examples/clike/base/docs_examples/ShowExtendedBuiltins
PID = 98106
After Swap: a=2 b=1
```
