# Pascal Overview

### Front End
The Pascal front end tokenises source text with a hand‑written lexer that recognises keywords such as `begin`, `record`, `set`, `while` and `writeln`【F:src/Pascal/lexer.c†L10-L33】. The parser builds an abstract syntax tree (AST) for programs, units, and type declarations, supporting constructs like enums and records.

### Bytecode Compiler
The compiler walks the AST and emits bytecode instructions into a `BytecodeChunk`. It maintains scope information for locals, loop handling, and constant pools【F:src/compiler/compiler.c†L1-L72】.

### Virtual Machine
The VM executes the bytecode on a stack-based interpreter. It provides helper routines for stack management and debugging and integrates with built‑in procedures through a dispatch table【F:src/vm/vm.c†L1-L78】【F:src/backend_ast/builtin.c†L35-L77】.

## The Language

Pscal implements a substantial subset of classic Pascal:

* **Basic types** – integers (`ShortInt` through `Int64`), real numbers (`Single`, `Double`, `Extended`), boolean, char, string, enumerations, sets and records.
* **Control flow** – `if`, `case`, `for`, `while`, `repeat…until`, `break`, and `goto` (labels are declared per routine and cannot be targeted from other procedures).
* **Operators** – arithmetic/logic expressions plus compound assignments (`+=`, `-=`) that expand to in-place addition or subtraction.
* **Subroutines** – functions and procedures with local variables and parameters.
* **Units** – separate compilation modules that export types, variables and routines.
* **Threading and Synchronization** – `spawn` starts a parameterless procedure in a new thread and returns its id; `join` waits for that thread to finish. Prefer `CreateThread(@Proc, arg)` and `WaitForThread(t)` when you need to pass a pointer argument; `WaitForThread` returns `0` on success and clears the stored status so idle workers can be reused immediately. The `Threading` unit adds helpers such as `ThreadOptionsNamed('worker')`, `SpawnBuiltinNamed('delay', 'worker')`, and `PoolSubmitNamed('delay', 'queue')` so Pascal code can forward names or queue-only flags to `ThreadSpawnBuiltin`/`ThreadPoolSubmit`, alongside simple wrappers like `ThreadStatsCount` or `ThreadStatusOk` for asynchronous monitoring. `mutex` and `rcmutex` create standard or recursive mutexes and return ids, while `lock`, `unlock`, and `destroy` manage their lifecycle.

Examples
- See `Examples/pascal/base/ThreadsProcPtrDemo` for a compact demonstration of:
  - Procedure/function pointers (including indirect calls), and
  - `CreateThread(@Worker, argPtr)`/`WaitForThread(t)` passing a pointer argument.
- `Examples/pascal/base/ClosureSafeCapture` walks through a mini league table
  where nested procedures track team scores while staying inside the defining
  scope, so the compiler accepts the captures.
- `Examples/pascal/base/ClosureEscapeError` intentionally fails to compile,
  demonstrating the "closure captures a local value that would escape its
  lifetime" diagnostic when a delayed callback keeps stack-allocated state.
- `Examples/pascal/base/ClosureEscapingWorkaround` shows how to enqueue
  stateful callbacks safely by boxing the payload on the heap and passing it to
  a global handler instead of capturing local variables.

Example programs: (Examples in this document can be found in Examples/Pascal/base/docs_examples)

```pascal
program Demo;
uses MathLib, StringUtil;
var
  r: real;
begin
  r := ArcSin(0.5);
  writeln('ArcSin(0.5)=', r:0:4);
  writeln(ReverseString('pscal'));
end.
```

### Records and Sets Example

```pascal
program RecordsSets;

type
  Day = (Mon, Tue, Wed, Thu, Fri, Sat, Sun);
  DaySet = set of Day;
  Person = record
    name: string;
    busy: DaySet;
  end;

var
  p: Person;
  d: Day;
begin
  p.name := 'Alice';
  p.busy := [Mon, Wed];
  for d := Mon to Sun do
    if d in p.busy then
      writeln(p.name, ' is busy on ', d);
end.
```

### Interfaces and Virtual Dispatch

Interfaces capture the receiver pointer and a method table so concrete records
with virtual methods can be passed around abstractly. During compilation
`validateInterfaceImplementation` ensures the target record provides every
required virtual, so casts now fail at compile time when a method is missing
rather than deferring to runtime.【F:src/compiler/compiler.c†L1860-L1963】 The
code generator then emits `CALL_HOST HOST_FN_BOX_INTERFACE`, passing the class
and interface names so `vmHostBoxInterface` can box the receiver and initialise
its table; subsequent calls dispatch through
`HOST_FN_INTERFACE_LOOKUP`/`vmHostInterfaceLookup` using the stored slot
indices.【F:src/compiler/compiler.c†L8000-L8066】【F:src/vm/vm.c†L2990-L3247】

```pascal
program InterfaceDemo;

type
  ILogger = interface
    procedure Log(const msg: string);
  end;

  TConsoleLogger = record
    procedure Log(const msg: string); virtual;
  end;

procedure TConsoleLogger.Log(const msg: string);
begin
  writeln('[console] ', msg);
end;

var
  logger: ILogger;
  concrete: ^TConsoleLogger;
begin
  new(concrete);
  logger := ILogger(concrete);
  logger.Log('ready');
end.
```

Running the program prints the log message instead of the boxed receiver pointer:

```
[console] ready
```

### Interface assertions

Boxed interface values retain the identity of the concrete record they wrap.
You can recover the underlying receiver (or fail fast) with the new assertion
syntax:

```pascal
var
  logger: ILogger;
  concrete: ^TConsoleLogger;
begin
  new(concrete);
  logger := ILogger(concrete);
  concrete := logger as TConsoleLogger; { raises an error if the receiver is not TConsoleLogger }
  concrete^.Log('asserted receiver');
end.
```

Both `expr as TRecord` and `expr is TRecord` perform the same runtime check.
The parser lowers them into assertion nodes, the compiler emits
`HOST_FN_INTERFACE_ASSERT`, and `vmHostInterfaceAssert` compares the stored
class identity before handing the receiver back to Pascal code. Mismatches raise
clear runtime errors, while successful assertions return the boxed pointer for
immediate use.【F:src/Pascal/parser.c†L3755-L3787】【F:src/compiler/compiler.c†L6863-L6903】【F:src/vm/vm.c†L3250-L3325】

## Builtins

Builtins are implemented in C and exposed to Pascal through a lookup table【F:src/backend_ast/builtin.c†L35-L176】. They cover:

* **Math** – `abs`, `cos`, `sin`, `tan`, `exp`, `ln`, `sqrt`, `sqr`, `round`, `trunc`.
* **Strings** – `length`, `copy`, `pos`, `chr`, `ord`, `inttostr`, `formatfloat`, `realtostr`, `upcase`.
* **I/O** – `readln`, `writeln`, `readkey` (optional char parameter), `halt`, `gotoxy`, `whereX`, `textcolor`.
* **Files & streams** – `assign`, `reset`, `rewrite`, `close`, `eof`, memory stream helpers (`mstreamcreate`, `mstreamloadfromfile`, `mstreamsavetofile`, `mstreambuffer`, `mstreamappendbyte`, etc.).
* **Random numbers** – `randomize`, `random`.
* **System utilities** – `paramcount`, `paramstr`, `exit`, `ioresult`.
* **DOS compatibility** – directory and environment calls (`dosFindfirst`, `dosGetenv`, ...).

Example:

```pascal
program Randomize;

var
  n: integer;
begin
  randomize;
  n := random(10) + 1;
  writeln('Random number: ', n);
end.
```

## Included Units

### StringUtil
Provides simple string helpers such as reversing text【F:lib/pascal/StringUtil.pl†L1-L28】.

* `ReverseString(s: string): string` – returns the characters of `s` in reverse order.

Example:
```pascal
program ReverseWord;

uses StringUtil;
begin
  writeln(ReverseString('hello'));  // olleh
end.
```

### Base64
Implements Base64 encoding and decoding【F:lib/pascal/base64.pl†L1-L155】.

* `EncodeStringBase64(s: string): string`
* `DecodeStringBase64(s: string): string`

### CalculateArea
Provides geometry helpers【F:lib/pascal/calculatearea.pl†L1-L31】.

* `RectangleArea(len, width: real): real`
* `CircleArea(radius: real): real`
* `TriangleArea(side1, side2, side3: real): real` – uses Heron’s formula.

### CRT
ANSI terminal control compatible with Turbo Pascal’s CRT unit【F:lib/pascal/crt.pl†L26-L57】.

* Screen and cursor routines: `ClrScr`, `GotoXY`, `ClrEol`, `Window`, `WhereX`, `WhereY`.
* Text attributes: `TextColor`, `TextBackground`, `TermBackground`, `NormVideo`, `HighVideo`, `LowVideo`, `InvertColors`, `BoldText`, `BlinkText`.
* Miscellaneous: `Delay`, `Beep`, `KeyPressed`, `SaveCursor`, `RestoreCursor`.

### Dos
Provides DOS‑style file and environment operations by wrapping builtins【F:lib/pascal/dos.pl†L1-L66】.

* `findFirst`, `findNext`
* `getFAttr`, `mkDir`, `rmDir`
* `getEnv`
* `getDate`, `getTime`
* `exec`

### MathLib
Compatibility shim for math routines【F:lib/pascal/mathlib.pl†L1-L80】.

- MathLib now forwards to VM builtins for all functions, keeping existing
  programs that `uses MathLib;` working without modification.
- Prefer calling the builtins directly in new code: `arctan`, `arcsin`,
  `arccos`, `cotan`, `power`, `log10`, `sinh`, `cosh`, `tanh`, `max`, `min`,
  `floor`, `ceil`.

Provided wrappers:
- Trigonometry: `ArcTan`, `ArcSin`, `ArcCos`, `Cotan`
- Exponentials: `Power`, `Log10`, `Sinh`, `Cosh`, `Tanh`
- Helpers: `Max`, `Min`, `Floor`, `Ceil`

Helpers exported by MathLib for common constants:
- `PiValue`, `EValue`, `Ln2Value`, `Ln10Value`, `TwoPiValue`, `PiOver2Value`

### mylib
Example user unit exporting a record type and routines【F:lib/pascal/mylib.pl†L1-L40】.

* `Greet(name: string)`
* `Add(a, b: integer): integer`
* `GetPi: real`
* `TPerson` record and `GlobalCounter` variable

### SysUtils
Common utility routines【F:lib/pascal/sysutils.pl†L1-L132】.

* `UpperCase`, `LowerCase`, `Trim`, `QuotedStr`
* `FileExists`

Example:
```pascal
program CheckFile;

uses SysUtils;
begin
  if FileExists('data.txt') then
    writeln(UpperCase('file found'))
  else
    writeln(UpperCase('file not found'));
end.
```

```
% pascal CheckFile 
FILE NOT FOUND
% touch data.txt
% pascal check_file
FILE FOUND
```

## Summary
Pscal offers a Pascal environment with a modular architecture, a rich set of builtins, and reusable units for console I/O, math, string handling and system access. These tools make it suitable both for educational purposes and for experimenting with Pascal‑like language design.

For a complete language specification, see
[`pascal_language_reference.md`](pascal_language_reference.md).
