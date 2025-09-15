# Rea method dispatch runtime investigation

Summary:
- Qualified calls now compile to direct calls; method invocations like `obj.method(args)` translate to the internal `Class_method(obj, args)` form and work in disassembly.
- A runtime stack underflow occurs at program start when executing method_calls.

Next steps:
- Ensure annotateTypes sets receiver expression types in statement contexts before compileStatement type checks.
- Verify global variable initialization code path and constant indices with VM trace.
- Add unit test to execute a minimal class with one setter and call via variable and new-expression receivers.
- Once fixed, move method_calls back to Tests/rea and update expected outputs.

Tracking: this branch fix/rea-method-calls-runtime.
