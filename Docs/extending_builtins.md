# Extending VM Built-ins

Pscal allows additional built-in routines to be linked into the virtual
machine at build time.  This makes it easy to expose host functionality
without modifying the core source tree.  Any `*.c` files placed in
`src/ext_builtins` are automatically compiled and linked into the
executables.

## Creating a new builtin

Drop a C file into `src/ext_builtins` that defines one or more builtin
handlers and registers them in a `registerExtendedBuiltins` function.
The repository includes `src/ext_builtins/getpid.c`, which exposes the
process ID through a `GetPid` Pascal function:

```c
#include <unistd.h>
#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinGetPid(struct VM_s* vm, int arg_count, Value* args) {
    (void)vm; (void)args;
    return arg_count == 0 ? makeInt(getpid()) : makeInt(-1);
}

void registerExtendedBuiltins(void) {
    registerBuiltinFunction("GetPid", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("getpid", vmBuiltinGetPid);
}
```

Any additional files added to `src/ext_builtins` will be picked up the
next time you run CMake and `make`.

## Using the builtin

After rebuilding, the new routine can be invoked from Pascal code:

```pascal
program ShowPID;
begin
  writeln('PID = ', GetPid());
end.
```

Running the program prints the current process ID:

```sh
$ build/bin/pscal Examples/Pascal/show_pid.p
PID = 12345
```

The same builtin is available to the C-like front end.  An equivalent
program can be written as:

```c
int main() {
  printf("PID = %d\n", getpid());
  return 0;
}
```

Running it with the `clike` compiler yields the same output:

```sh
$ build/bin/clike Examples/CLike/show_pid.c
PID = 12345
```
