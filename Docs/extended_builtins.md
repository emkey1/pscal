# Extending VM Built-ins

Pscal allows additional built‑in routines to be linked into the virtual
machine at build time.  This makes it easy to expose host functionality
without modifying the core source tree.  Optional built‑ins live under
`src/ext_builtins` and are grouped into categories.

For a catalog of existing VM routines, see
[`pscal_vm_builtins.md`](pscal_vm_builtins.md).

## Available extended built-ins

The project currently ships several optional built‑in groups:

| Category | Location | Built-ins |
| -------- | -------- | --------- |
| **Math** | `src/ext_builtins/math` | `Factorial`, `Fibonacci`, `MandelbrotRow`, `Chudnovsky` |
| **System** | `src/ext_builtins/system` | `FileExists`, `GetPid`, `RealTimeClock`, `Swap` |
| **Strings** | `src/ext_builtins/strings` | (none yet) |
| **Yyjson** | `src/ext_builtins/yyjson` | `YyjsonRead`, `YyjsonReadFile`, `YyjsonDocFree`, `YyjsonFreeValue`, `YyjsonGetRoot`, `YyjsonGetKey`, `YyjsonGetIndex`, `YyjsonGetLength`, `YyjsonGetType`, `YyjsonGetString`, `YyjsonGetNumber`, `YyjsonGetInt`, `YyjsonGetBool`, `YyjsonIsNull` |
| **User** | `src/ext_builtins/user` | (user-defined) |

Individual categories can be enabled or disabled at configure time with
the following CMake options (all default to `ON`):

```
-DENABLE_EXT_BUILTIN_MATH=ON/OFF
-DENABLE_EXT_BUILTIN_STRINGS=ON/OFF
-DENABLE_EXT_BUILTIN_SYSTEM=ON/OFF
-DENABLE_EXT_BUILTIN_USER=ON/OFF
-DENABLE_EXT_BUILTIN_YYJSON=ON/OFF
```

### Yyjson built-ins

The `yyjson` category wraps the bundled [yyjson](https://github.com/ibireme/yyjson) library and exposes helpers for parsing documents, walking objects and arrays, and converting primitive values. Each routine operates on integer handles returned by `YyjsonRead` or the various query helpers; release value handles with `YyjsonFreeValue` and dispose of documents with `YyjsonDocFree` when they are no longer needed.

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
$ build/bin/pascal Examples/Pascal/ShowExtendedBuiltins
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
$ build/bin/clike Examples/clike/docs_examples/ShowExtendedBuiltins
PID = 98106
After Swap: a=2 b=1
```

