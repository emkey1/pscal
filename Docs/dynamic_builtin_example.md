# Dynamic Builtins

Pscal can register extra built-in functions at runtime.  This lets host
applications expose optional functionality without recompiling or modifying
the interpreter.

## TestDynamicBuiltin example

The source tree includes a sample builtin named `TestDynamicBuiltin`.  The
function is only registered when the environment variable
`PSC_TEST_DYNAMIC_BUILTIN` is set.  When available, it returns the integer
`123`.

1. **Write a Pascal program**:

   ```pascal
   program DemoDynamicBuiltin;
   begin
     writeln('Result = ', TestDynamicBuiltin());
   end.
   ```

2. **Run without the variable**:

   ```bash
   $ build/bin/pscal demo_dynamic.p
   L0: Compiler Error: Undefined function 'testdynamicbuiltin'.
   ```

3. **Run with the variable**:

   ```bash
   $ PSC_TEST_DYNAMIC_BUILTIN=1 build/bin/pscal demo_dynamic.p
   Compilation successful. Bytecode size: 17 bytes, Constants: 4
   Result = 123
   ```

Applications can use this pattern to expose debugging routines, optional
features or site-specific extensions.  Each builtin is registered with
`registerVmBuiltin` and can be called from Pascal code like any other builtin
when enabled.
