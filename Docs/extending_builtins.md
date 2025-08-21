# Extending VM Built-ins

Pscal allows additional built-in routines to be linked into the virtual
machine at build time.  This makes it easy to expose host functionality
without modifying the core source tree.  Any `*.c` files placed in
`src/ext_builtins` are automatically compiled and linked into the
executables.

## Creating a new builtin

Drop a C file into `src/ext_builtins` that defines one or more builtin
handlers. Each file typically provides a small registration helper, and a
separate `registerExtendedBuiltins` function ties them together. The
repository includes `src/ext_builtins/getpid.c`, which exposes the process
ID through a `GetPid` Pascal function:

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

A second example shows how to accept VAR parameters.  `src/ext_builtins/swap.c`
defines a simple procedure that swaps two variables and provides a helper
`registerSwapBuiltin`:

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

A tiny `register.c` file wires the helpers together by defining
`registerExtendedBuiltins`:

```c
void registerGetPidBuiltin(void);
void registerSwapBuiltin(void);

void registerExtendedBuiltins(void) {
    registerGetPidBuiltin();
    registerSwapBuiltin();
}
```

Any additional files added to `src/ext_builtins` will be picked up the
next time you run CMake and `make`, assuming you have also added it to the 
src/ext_builtins/register.c file, as shown above.

## Using the builtins

After rebuilding, the new routines can be invoked from Pascal code:

```pascal
program ShowBuiltins;

type PInt = ^integer;
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

Running the program prints the current process ID:

```sh
$ build/bin/pscal Examples/Pascal/show_builtins.p
PID = 12345
After Swap: a=2 b=1
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
$ build/bin/clike Examples/CLike/show_pid.cl
PID = 12345
```

## Performance implications
Lets take the fibonacci examples in the Examples/Clike directory.

First we'll time fibonacci_native.cl on my M1 MacBook Pro...
```sh
time ../../build/bin/clike fibonacci_ext.cl
Please enter an integer value for fibonacci: 92
0
1
1
2
3
5
8
13
21
34
55
89
144
233
377
610
987
1597
2584
4181
6765
10946
17711
28657
46368
75025
121393
196418
317811
514229
832040
1346269
2178309
3524578
5702887
...
fibonacci_ext.cl  0.01s user 0.01s system 0% cpu 3.058 total

```

Now we'll time fibonacci_ext.cl

```sh
time ../../build/bin/clike fibonacci_native.cl
Please enter an integer value for fibonacci: 92
0
1
1
2
3
5
8
13
21
34
55
89
144
233
377
610
987
1597
2584
4181
6765
10946
17711
28657
46368
75025
121393
196418
317811
514229
832040
1346269
2178309
3524578
5702887
...
fibonacci_native.cl  0.02s user 0.01s system 0% cpu 3.157 total
```

The performance improvement is small using the extended buildin C fibonacci function from 
the src/ext_builtins directory, but it is there.  Other tasks might see a more substantial
improvement.
