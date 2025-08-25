# Extending VM Built-ins

Pscal can link optional C helpers into the virtual machine at build time.  These
extensions live under `src/ext_builtins/` and are organised by category to keep
unrelated routines separate:

```
src/ext_builtins/
  math/
  strings/
  system/
  user/
```

The `user/` directory is reserved for project-specific extensions and is empty by default.


Each subdirectory contains its own `register.c` that exposes an init function
(`pascal_ext_<category>_init`) and optional CMake toggle.  The top-level
`src/ext_builtins/register.c` calls these init functions when the corresponding
option is enabled.

## Build options

`src/ext_builtins/CMakeLists.txt` defines per-category switches which default to
`ON`:

```
option(ENABLE_EXT_BUILTIN_MATH "Build math extended builtins" ON)
option(ENABLE_EXT_BUILTIN_STRINGS "Build string extended builtins" ON)
option(ENABLE_EXT_BUILTIN_SYSTEM "Build system extended builtins" ON)
option(ENABLE_EXT_BUILTIN_USER "Build user extended builtins" ON)
```

Disable a category at configure time with e.g.:

```sh
cmake -DENABLE_EXT_BUILTIN_STRINGS=OFF ..
```

## Adding a builtin

1. Choose the appropriate category directory or create a new one.
2. Drop a `*.c` file that implements the VM handler and a small registration
   helper.
3. Include the helper in that directory's `register.c`.

### Example – `ReverseString`

`src/ext_builtins/strings/reversestring.c` exposes a function that returns the
characters of a string in reverse order:

```c
static Value vmReverseString(struct VM_s* vm, int argc, Value* args) {
    if (argc != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "ReverseString expects a single string argument");
        return makeNull();
    }
    const char* s = AS_CSTRING(args[0]);
    size_t len = strlen(s);
    char* out = GC_MALLOC_ATOMIC(len + 1);
    for (size_t i = 0; i < len; ++i) out[i] = s[len - 1 - i];
    out[len] = '\0';
    return makeString(out, len);
}

void registerReverseString(void) {
    registerBuiltinFunction("ReverseString", AST_FUNCTION_DECL, NULL);
    registerVmBuiltin("reversestring", vmReverseString);
}
```

The `strings/register.c` file wires the category together:

```c
void registerReverseString(void);

void pascal_ext_strings_init(void) {
    registerReverseString();
}
```

After rebuilding, the builtin is available from Pascal code via the `StringUtil`
unit:

```pascal
program Demo;
uses StringUtil;
begin
  writeln(ReverseString('pscal'));
end.
```

For a catalogue of existing routines, see
[`pscal_vm_builtins.md`](pscal_vm_builtins.md).

