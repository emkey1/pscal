# Aether base examples

This directory contains small Aether programs that run with the current
bootstrap frontend:

```sh
./build/bin/aether Examples/aether/base/hello
./build/bin/aether Examples/aether/base/control_flow
./build/bin/aether Examples/aether/base/effects_contracts
./build/bin/aether Examples/aether/base/contracts
./build/bin/aether Examples/aether/base/pure_functions
```

These examples stay within the currently supported Aether Core subset:

- `const` and `let` declarations with types
- `fn ... -> Type`
- `ret`
- `if` and `while` without mandatory parentheses
- `fx { ... }`
- known effectful builtins such as `write`, `writeln`, `printf`, `readln`,
  `halt`, and thread-launch helpers require `fx`
- `@pure` now rejects direct effectful builtin calls and direct calls into
  non-pure Aether functions
- `@pre` and `@post` lowered into runtime guards
- annotation forms such as `@pure` and `@cost` preserved as metadata comments

The backend remains the shared PSCAL compiler and VM.
