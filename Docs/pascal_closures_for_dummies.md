# Closures (and Friends) for PSCAL Pascal Developers

Welcome! This guide explains how the Pascal front end now supports Go-style closures and interface payloads without making you wade through compiler internals. If you just want to return nested routines, stash them in records, or pass them around via interfaces, start here.

## What Changed?

1. **Semantic analysis records which nested routines capture locals _and_ escape.** Instead of failing immediately, the compiler tags those routines so later stages can allocate closure environments.【F:src/Pascal/semantic.c†L207-L289】
2. **Closure values are real heap-backed objects.** When you take `@NestedProc`, the compiler emits a host call that bundles the procedure entry point together with boxed copies (or references) of the captured locals.【F:src/compiler/compiler.c†L960-L1003】【F:src/core/utils.c†L1054-L1065】
3. **The VM understands closures and interfaces.** Indirect calls pull the environment out of the closure payload, while Go-style interface casts box a receiver pointer plus its method table in a reusable payload.【F:src/vm/vm.c†L6871-L7002】【F:src/vm/vm.c†L2712-L2893】

Taken together, nested routines can finally outlive their defining scope, and interface values no longer require hand-written record wrappers.

## Captures, Escapes, and Boxing Explained

- During semantic analysis the compiler records each captured slot’s index and whether it was passed by reference. Escaping routines are marked so code generation knows to build a heap environment rather than relying on the caller’s stack.【F:src/Pascal/semantic.c†L207-L324】
- At runtime a closure environment is just a tiny struct: a refcount, a slot array, and a pointer back to the routine’s symbol so the VM can respect which captures were by-reference.【F:src/core/utils.c†L1000-L1065】
- When the closure is created the VM pops each captured value. `VAR` parameters stay as shared pointers, while normal locals are copied into freshly allocated heap cells so they remain valid after the parent returns.【F:src/vm/vm.c†L2635-L2709】

### Practical takeaway
You can now safely return or store nested routines that touch outer-scope variables. The environment lives on the heap and is reference counted, so multiple closures can share the same captured data without leaks.

## Everyday Closure Patterns

### Returning a Counter Function
```pascal
function MakeCounter(start: integer): function(): integer;
var
  value: integer;
  function Next: integer;
  begin
    value := value + 1;
    Next := value;
  end;
begin
  value := start;
  MakeCounter := @Next;
end;
```
The nested `Next` procedure captures `value`. When `MakeCounter` returns, the compiler emits a closure that keeps `value` alive. You can store and call it later—even in global variables or records.【F:Tests/compiler/pascal/cases/closure_capturing_store_return.pas†L1-L35】

### Storing Closures in Globals or Records
```
var
  Stored: function(): integer;

Stored := MakeCounter(2);
writeln(Stored()); // prints 3 on the first call
```
Because the closure owns its environment, using it after `MakeCounter` has returned is safe and produces the expected sequence.【F:Tests/compiler/pascal/cases/closure_capturing_store_return.pas†L19-L34】

### Limitations to Remember
- **Threads now retain closure environments.** `CreateThread` accepts capturing closures, keeps their environments alive until the worker finishes, and releases them automatically; the `Threading` unit’s `SpawnProcedure` helper lets Pascal code forward nested routines without building custom payload records.【F:src/vm/vm.c†L2588-L2676】【F:lib/pascal/threading.pl†L11-L61】
- **Global scope cannot host capturing closures.** Attempting to take the address of a capturing routine declared at the top level still produces a compiler error.【F:src/compiler/compiler.c†L965-L1003】

## Interface Payloads Without Boilerplate

Casting a record pointer to an interface now boxes two things for you: the receiver pointer and its method table. Calls through the interface push the receiver back on the stack and jump via the stored address, mirroring Go’s interface dispatch model.【F:src/compiler/compiler.c†L7244-L7262】【F:src/vm/vm.c†L2712-L2893】

### Example Flow
1. The compiler emits `CALL_HOST HOST_FN_BOX_INTERFACE`, providing the vtable pointer, receiver pointer, and the interface type name.【F:src/compiler/compiler.c†L7244-L7262】
2. The VM builds a closure-style payload with two slots: slot 0 holds the receiver pointer, slot 1 holds the method table array.【F:src/vm/vm.c†L2712-L2794】
3. When you invoke `iface.Log('hi')`, the VM unpacks the payload, fetches the right method entry, pushes the receiver, and performs an indirect call using the stored address.【F:src/vm/vm.c†L2795-L2893】

The net effect: you can assign different concrete records to the same interface variable without hand-writing glue code.

## Debugging Tips
- If a closure crashes when invoked, check the capture count mismatch error—the VM reports when the emitted metadata and runtime payload disagree.【F:src/vm/vm.c†L2666-L2709】
- Use the bytecode disassembler to confirm the closure host calls are present; `CALL_HOST HOST_FN_CREATE_CLOSURE` will appear right after the captured values are pushed.【F:src/compiler/compiler.c†L994-L1003】
- Interfaces rely on the method table exported from your record’s virtual declarations. Missing or empty tables trigger the “Interface method table is not an array” runtime guard.【F:src/vm/vm.c†L2825-L2843】

## Where to Experiment Next
- The `Examples/pascal/base/ClosureEscapingWorkaround` demo still compiles, but you can now simplify it by returning the nested handlers directly.
- Try building a small task scheduler that stores closures in a queue, or define a `Runnable` interface and box different record implementations—the runtime already handles the plumbing.

Happy hacking!
