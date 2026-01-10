# Closures (and Friends) for PSCAL Pascal Developers

Welcome! This guide explains how the Pascal front end now supports Go-style closures and interface payloads without making you wade through compiler internals. If you just want to return nested routines, stash them in records, or pass them around via interfaces, start here.

## What Changed?

1. **Semantic analysis records which nested routines capture locals _and_ escape.** Instead of failing immediately, the compiler tags those routines so later stages can allocate closure environments.【F:src/Pascal/semantic.c†L207-L289】
2. **Closure values are real heap-backed objects.** When you take `@NestedProc`, the compiler emits a host call that bundles the procedure entry point together with boxed copies (or references) of the captured locals.【F:src/compiler/compiler.c†L960-L1003】【F:src/core/utils.c†L1117-L1182】
3. **The VM understands closures and interfaces.** `vmHostCreateClosure` assembles the capture payload, while interface casts rely on `vmHostBoxInterface`, dispatch through `vmHostInterfaceLookup`, and assert concrete types via `vmHostInterfaceAssert`.【F:src/vm/vm.c†L2681-L3325】

Taken together, nested routines can finally outlive their defining scope, and interface values no longer require hand-written record wrappers.

## Captures, Escapes, and Boxing Explained

- During semantic analysis the compiler records each captured slot’s index and whether it was passed by reference. Escaping routines are marked so code generation knows to build a heap environment rather than relying on the caller’s stack.【F:src/Pascal/semantic.c†L207-L324】
- At runtime a closure environment is just a tiny struct: a refcount, a slot array, and a pointer back to the routine’s symbol so the VM can respect which captures were by-reference.【F:src/core/utils.c†L1117-L1167】
- When the closure is created the VM pops each captured value. `VAR` parameters stay as shared pointers, while normal locals are copied into freshly allocated heap cells so they remain valid after the parent returns.【F:src/vm/vm.c†L2681-L2755】

### Practical takeaway
You can now safely return or store nested routines that touch outer-scope variables. The environment lives on the heap and is reference counted, so multiple closures can share the same captured data without leaks.

## Quick Demo

For a compact walkthrough, see `Examples/pascal/base/docs_examples/GoStyleClosureInterfaceDemo`.
It returns a nested function that captures its local counter, stores the resulting
closure inside a record, and boxes that record behind an interface. Each call goes
through the interface method, unpacks the receiver, and invokes the captured
closure so the counter keeps advancing even after the factory routine has exited.
During dispatch the VM now seeds the implicit `myself` pointer, letting the record
method reach its own fields without any extra scaffolding.

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
- **Threads now retain closure environments.** `CreateThread` accepts capturing closures, keeps their environments alive until the worker finishes, and releases them automatically; the `Threading` unit’s `SpawnProcedure` helper lets Pascal code forward nested routines without building custom payload records. The guard inside `vmHostCreateThreadAddr` enforces that callers pass a closure or procedure pointer, retaining the environment until the job is queued.【F:src/vm/vm.c†L2589-L2678】【F:lib/pascal/threading.pl†L11-L61】
- **Global scope cannot host capturing closures.** Attempting to take the address of a capturing routine declared at the top level still produces a compiler error.【F:src/compiler/compiler.c†L965-L1003】

## Interface Payloads Without Boilerplate

Casting a record pointer to an interface now boxes two things for you: the receiver pointer and its method table. Calls through the interface push the receiver back on the stack and jump via the stored address, mirroring Go’s interface dispatch model.【F:src/compiler/compiler.c†L8000-L8066】【F:src/vm/vm.c†L2990-L3247】

### Example Flow
1. The compiler emits `CALL_HOST HOST_FN_BOX_INTERFACE`, providing the vtable pointer, receiver pointer, and the interface type name.【F:src/compiler/compiler.c†L8000-L8066】
2. `vmHostBoxInterface` builds a closure-style payload with slots for the receiver, method table, and lowered class identity.【F:src/vm/vm.c†L2990-L3126】
3. When you invoke `iface.Log('hi')`, `vmHostInterfaceLookup` unpacks the payload, fetches the right method entry, pushes the receiver, and performs an indirect call using the stored address.【F:src/vm/vm.c†L3129-L3247】

The net effect: you can assign different concrete records to the same interface variable without hand-writing glue code.

### Go-Style Interface Roundtrip

The compiler emits the interface boxing helper automatically—you can return a record value, assign it to an interface, and call methods with no boilerplate:

```pascal
program GoInterfaceDemo;

type
  Logger = interface
    procedure Log(const msg: string);
  end;

type
  ConsoleLogger = record
    tag: string;
    procedure Log(const msg: string);
  end;

procedure ConsoleLogger.Log(const msg: string);
begin
  writeln(tag, ': ', msg);
end;

function MakeLogger(const tag: string): Logger;
var
  inst: ConsoleLogger;
begin
  inst.tag := tag;
  MakeLogger := inst;
end;

var L: Logger;
begin
  L := MakeLogger('demo');
  L.Log('hello world');
end.
```
No pointer aliases or `virtual` annotations required—the boxing flow described above kicks in automatically whenever a record is assigned to an interface.

## Debugging Tips
- If a closure crashes when invoked, check the capture count mismatch error—the VM reports when the emitted metadata and runtime payload disagree.【F:src/vm/vm.c†L2724-L2755】
- Use the bytecode disassembler to confirm the closure host calls are present; `CALL_HOST HOST_FN_CREATE_CLOSURE` will appear right after the captured values are pushed.【F:src/compiler/compiler.c†L994-L1003】
- Interfaces rely on the method table exported from your record’s virtual declarations. Missing or empty tables trigger the “Interface method table is not an array” runtime guard inside `vmHostInterfaceLookup`.【F:src/vm/vm.c†L3179-L3243】

## Where to Experiment Next
- The `Examples/pascal/base/ClosureEscapingWorkaround` demo still compiles, but you can now simplify it by returning the nested handlers directly.
- Try building a small task scheduler that stores closures in a queue, or define a `Runnable` interface and box different record implementations—the runtime already handles the plumbing.

Happy hacking!
