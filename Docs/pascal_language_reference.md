# Pascal Language Specification

### **Introduction and Design Philosophy**

This document specifies the Pascal-like language, a front end for the PSCAL virtual machine (VM). The language is designed to offer a classic, Pascal-style syntax for developers, particularly those familiar with Turbo Pascal or Delphi. It compiles down to the same bytecode as the C-like front end, allowing for a choice of syntactic flavors while targeting a single, unified runtime environment.

The language is strongly typed and emphasizes readability and structured programming. It supports a variety of traditional Pascal features, including a robust type system with records, arrays, enums, sets, and pointers, as well as a modular programming system using units.

### **Lexical Structure**

#### **Comments**

* **Brace-delimited comments:** Start with `{` and end with `}`. These can be nested.
* **Parenthesis-star comments:** Start with `(*` and end with `*)`. These can also be nested.
* **Single-line comments:** Start with `//` and extend to the end of the line.

Comments may also carry simple directives. Placing `override builtin` inside a comment immediately before or on the same line as a routine declaration tells the compiler that the upcoming user-defined routine intentionally replaces a builtin. For example:

```pascal
function Fibonacci(n: Integer): LongInt; // override builtin fibonacci
```

The directive works with `//`, `{ ... }`, or `(* ... *)` comment styles. If you omit the identifier after `override builtin`, the next builtin routine that would have generated an override warning is silenced. Supplying an identifier (case-insensitive) restricts suppression to that specific routine name.

#### **Keywords**

The following are reserved keywords and cannot be used as identifiers:

* `and`, `array`, `begin`, `break`
* `case`, `const`, `do`, `div`, `downto`
* `else`, `end`, `enum`, `false`, `for`, `function`, `goto`
* `if`, `implementation`, `in`, `inline`, `initialization`, `interface`, `join`, `label`
* `mod`, `nil`, `not`
* `of`, `or`, `out`
* `procedure`, `program`
* `read`, `readln`, `record`, `repeat`
* `set`, `shl`, `shr`, `spawn`, `xor`
* `then`, `to`, `true`, `type`
* `unit`, `until`, `uses`
* `var`, `while`, `write`, `writeln`

#### **Identifiers**

Identifiers are used for the names of variables, functions, types, and other program elements. They must begin with a letter or an underscore (`_`) and can be followed by any number of letters, digits, or underscores. Identifiers are case-insensitive.

#### **Literals**

* **Integer Literals:** Can be decimal (e.g., `123`) or hexadecimal (e.g., `$7B`).
* **Real Literals:** Written with a decimal point (e.g., `3.14`) and may include an exponent (e.g., `1.23E-4`).
* **Character Literals:** A single character enclosed in single quotes (e.g., `'a'`). Character codes can also be specified using a `#` followed by an integer (e.g., `#65` for 'A').
* **String Literals:** A sequence of characters enclosed in single quotes (e.g., `'hello'`). To include a single quote within a string, it must be doubled (e.g., `'It''s a string'`). C-style escape sequences (`\n`, `\t`, etc.) are also supported.

### **Data Types**

The language supports a variety of built-in data types:

| Keyword | VM Type | Description |
| :--- | :--- | :--- |
| `ShortInt` | `TYPE_INT8` | 8-bit signed integer. |
| `SmallInt` | `TYPE_INT16` | 16-bit signed integer. |
| `Integer` | `TYPE_INT32` | 32-bit signed integer. |
| `LongInt`, `Int64` | `TYPE_INT64` | 64-bit signed integer. |
| `Byte` | `TYPE_BYTE` | 8-bit unsigned integer. |
| `Word` | `TYPE_WORD` | 16-bit unsigned integer. |
| `Cardinal` | `TYPE_UINT32` | 32-bit unsigned integer. |
| `Single` | `TYPE_FLOAT` | 32-bit floating-point number. |
| `Real`, `Double` | `TYPE_DOUBLE` | 64-bit floating-point number. |
| `Extended` | `TYPE_LONG_DOUBLE` | Extended precision floating-point number. |
| `Char` | `TYPE_CHAR` | Unicode code point. |
| `String` | `TYPE_STRING` | Dynamic-length or fixed-length string. |
| `Boolean` | `TYPE_BOOLEAN` | `True` or `False`. |
| `Text`, `File` | `TYPE_FILE` | Represents a file handle. |
| `Thread` | `TYPE_THREAD` | A handle identifying a VM thread. |
| `MStream` | `TYPE_MEMORYSTREAM` | A dynamic in-memory stream of bytes. |

`ThreadSpawnBuiltin` and `ThreadPoolSubmit` expose the VM's worker pool directly.  The standard `Threading` unit provides small helpers so you can forward thread names or queue-only flags without hand-crafting records.  For example, `SpawnBuiltinNamed('delay', 'worker')` wraps `ThreadSpawnBuiltin` with a name, while `ThreadOptionsQueue('loader')` returns a record suitable for queuing a job via `ThreadPoolSubmit`.  Additional wrappers such as `ThreadStatusOk(t, true)` and `ThreadStatsCount` mirror the VM builtins for checking results and sampling pool metrics.

### **Variables and Constants**

#### **Variable Declarations**

Variables must be declared in a `var` block before they are used. The syntax is:

```pascal
var
  identifier_list: type;
```

**Example:**

```pascal
var
  x, y: integer;
  name: string;
  isFinished: boolean;
```

#### **Constant Declarations**

Constants are declared in a `const` block. Their values must be known at compile time.

```pascal
const
  MAX_SIZE = 100;
  PI = 3.14159;
  GREETING = 'Hello';
```

### **Expressions and Operators**

The language supports a standard set of operators with Pascal-like precedence.

| Precedence | Operator | Description |
| :--- | :--- | :--- |
| 1 | `@`, `not` | Address-of, Logical NOT |
| 2 | `*`, `/`, `div`, `mod`, `and`, `shl`, `shr` | Multiplicative operators |
| 3 | `+`, `-`, `or`, `xor` | Additive operators |
| 4 | `=`, `<>`, `<`, `<=`, `>`, `>=`, `in` | Relational operators |

Compound assignments combine arithmetic with assignment. The parser recognises
`+=` and `-=` and lowers them to `lhs := lhs + rhs` and `lhs := lhs - rhs`
respectively; both forms require a numeric left-hand side.

### **Statements**

* **Assignment Statement:** `variable := expression;`
* **Compound Statements:** A sequence of statements enclosed between `begin` and `end`.
* **`if` Statements:**
    ```pascal
    if condition then
      statement
    else
      statement;
    ```
* **`while` Loops:**
    ```pascal
    while condition do
      statement;
    ```
* **`for` Loops:**
    ```pascal
    for loop_variable := start_value to end_value do
      statement;

    for loop_variable := start_value downto end_value do
      statement;
    ```
* **`repeat` Loops:**
    ```pascal
    repeat
      statement;
      ...
    until condition;
    ```
* **`case` Statements:**
    ```pascal
    case expression of
      constant_list: statement;
      ...
    else
      statement;
    end;
    ```
* **`break`:** Exits the current loop.
* **Label declarations:** `label StartPoint, Retry1;` — must appear before the first statement inside a routine or block.
* **`goto` Statements:** `goto StartPoint;` jumps to a label declared in the current routine.

#### Labels and `goto`

Labels let you mark positions inside a routine for `goto` statements. Declare
them near the top of the routine (or nested block) using a `label` section, then
attach the label with `LabelName:` immediately before a statement. `goto` may
target identifier-based labels or numeric labels, but the destination must live
inside the same procedure or function.

```pascal
procedure Scanner;
label Retry, Done;
var
  ch: Char;
begin
  Retry:
    Read(ch);
    if ch = '#' then
      goto Retry;
    if ch = '.' then
      goto Done;
    writeln('token=', ch);
    goto Retry;
  Done:
    writeln('scanner finished');
end;
```

Each label can be referenced more than once, but `goto` cannot cross routine
boundaries.

### **Procedures and Functions**

* **Procedures:** Subroutines that do not return a value.
    ```pascal
    procedure MyProcedure(parameter_list);
    begin
      // procedure body
    end;
    ```
* **Functions:** Subroutines that return a value.
    ```pascal
    function MyFunction(parameter_list): return_type;
    begin
      // function body
      MyFunction := return_value; // or result := return_value
    end;
    ```

### **User-Defined Types**

Types are defined in a `type` block.

* **`record`:**
    ```pascal
    type
      MyRecord = record
        field1: integer;
        field2: string;
      end;
    ```

    > **Note:** Traditional Turbo/Delphi `object`/`class` syntax is deliberately
    > omitted.  PSCAL Pascal achieves polymorphism with plain `record`s that may
    > declare `virtual` methods, closures, and Go-style interface boxing (see
    > `Docs/go_style_closure_interface_demo.md`).  Keywords such as `class`,
    > `object`, and `implements` are not recognised by the parser; use records +
    > interfaces instead.

#### Closures and Go-style composition

PSCAL Pascal’s “OO” story mirrors Go: you build concrete records with `virtual`
methods, capture state via nested routines, and expose behaviour through
interfaces.  From `Docs/go_style_closure_interface_demo.md`:

```pascal
type
  TClosureRunner = record
    labelText: string;
    nextValue: function: integer;  { closure producing the next tick }
    procedure Run; virtual;
  end;

procedure TClosureRunner.Run;
var
  runner: ^TClosureRunner;
  next: function: integer;
begin
  runner := myself;          { implicit when called via an interface }
  next := runner^.nextValue; { closure captured in factory }
  writeln(runner^.labelText, ' tick=', next());
end;

function MakeRunner(const name: string): IRunnable;
var
  current: integer;
  runner: ^TClosureRunner;

  function Next: integer;
  begin
    Inc(current, 2);
    Next := current;
  end;
begin
  New(runner);
  runner^.labelText := name;
  runner^.nextValue := @Next;     { closure escapes via record field }
  MakeRunner := IRunnable(runner); { boxed record behaves like Go interface }
end;
```

Nested closures (`Next`) capture lexical variables (`current`) even after the
factory returns.  When you cast a pointer to an interface (`IRunnable(runner)`),
the runtime boxes the record and its vtable, giving you Go-style method sets
without introducing new syntax.
* **`array`:**
    ```pascal
    type
      MyArray = array[1..10] of real;
    ```

#### Dynamic arrays

Dynamic arrays are declared with an open bound (`array of <Type>`) and default to
length zero. Allocate or resize them with `SetLength`:

```pascal
type
  TIntArray = array of Integer;

var
  values: TIntArray;
begin
  SetLength(values, 3);  { grows from 0 to 3 elements }
  values[0] := 10;
  values[1] := 20;
  values[2] := 30;
  SetLength(values, 5);  { preserves the first three slots and zero-initialises the tail }
  SetLength(values, 2);  { shrinks in place and keeps the leading elements }
end;
```

`SetLength` accepts multiple dimensions (`SetLength(matrix, rows, cols)`) and
retains overlapping contents when you resize nested arrays. Sibling references
made via assignment (`alias := values;`) continue to observe the updated data
because both variables point at the same heap allocation.

The usual helpers work with dynamic arrays:

* `Length(arr)` reports the current element count.
* `Low(arr)` is `0` when the array has elements and remains `0` for empty arrays.
* `High(arr)` evaluates to `Length(arr) - 1` for populated arrays and `-1` when
  the array is empty.

These intrinsics also operate on alias references so appending via
`SetLength(alias, Length(alias) + 1)` keeps `Low/High` in sync with the primary
variable.
* **Enumerated Types:**
    ```pascal
    type
      TColor = (cRed, cGreen, cBlue);
    ```
* **Sets:**
    ```pascal
    type
      TCharSet = set of char;
    ```
* **Pointers:**
    ```pascal
    type
      PInteger = ^integer;
    ```

### **Units and Modules**

The language supports modular programming through **units**. A unit is a separate source file that encapsulates constants, types, variables, procedures, and functions.

* **`unit`:** The first keyword in a unit file.
* **`interface`:** The public part of the unit, where declarations are made available to other programs or units.
* **`implementation`:** The private part of the unit, where the code for the interface's procedures and functions is written.
* **`uses`:** A clause to import other units.

### **Built-in Functions and Procedures**

The Pascal front end exposes the PSCAL VM's built-ins, including:

- File I/O: `Assign`, `Reset`, `Rewrite`, `Read`, `ReadLn`, `Write`, `WriteLn`, `Close`, `EOF`, `IOResult`, etc.
- String operations: `Length`, `Copy`, `Pos`, `Concat`, `UpCase`, `ReadKey`, conversions (`IntToStr`, `RealToStr`).
- Math: `Sin`, `Cos`, `Tan`, `Sqrt`, `Ln`, `Exp`, `Abs`, `Round`, `Trunc`, `Random`.
- Memory: `New`, `Dispose`.
- Console/text: `GotoXY`, `TextColor`, `TextBackground`, `ClrScr`, `WhereX`, `WhereY`.
- Concurrency (see below): `spawn`, `join`, `mutex`, `rcmutex`, `lock`, `unlock`, `destroy`.
- SDL-based graphics/sound (when built with `-DSDL=ON`): e.g., `InitGraph`, `InitGraph3D`, `CloseGraph`, `CloseGraph3D`, `UpdateScreen`, `GLClearColor`, `GLClear`, `GLClearDepth`, `GLMatrixMode`, `GLLoadIdentity`, `GLTranslatef`, `GLRotatef`, `GLScalef`, `GLBegin`/`GLEnd`, `GLColor3f`, `GLColor4f`, `GLVertex3f`, `GLNormal3f`, `GLLineWidth`, `GLDepthMask`, `GLDepthFunc`, `GLEnable`, `GLDisable`, `GLCullFace`, `GLShadeModel`, `GLLightfv`, `GLMaterialfv`, `GLMaterialf`, `GLColorMaterial`, `GLBlendFunc`, `GLViewport`, `GLDepthTest`, `GLSwapWindow`, `GLSetSwapInterval`, `SetRGBColor`, `DrawLine`, `FillRect`, `FillCircle`, `CreateTexture`, `UpdateTexture`, `DestroyTexture`, text helpers like `InitTextSystem(FontFileName, FontSize)`, `OutTextXY`, and audio: `InitSoundSystem`, `LoadSound`, `PlaySound`, `IsSoundPlaying`, `FreeSound`, `QuitSoundSystem`.
Note: SDL built-ins are available only in SDL-enabled builds. Headless CI typically skips these routines.

### **Threading and Synchronization**

The VM provides lightweight threads and mutexes for safe concurrency.

Thread lifecycle
- `spawn <ProcIdent>`: starts a new thread running a parameterless procedure, returns an `integer` thread id.
- `join <tid>`: blocks until the target thread finishes.

Mutex APIs
- `mutex(): integer`: create a standard (non-recursive) mutex; returns an id.
- `rcmutex(): integer`: create a recursive mutex (same thread may re‑acquire).
- `lock(<mid>)`: acquire the mutex with id `<mid>` (blocks until available).
- `unlock(<mid>)`: release a previously acquired mutex.
- `destroy(<mid>)`: permanently free the mutex; its id may be reused by future `mutex/rcmutex` calls.

Semantics and notes
- Threads share global variables. Use mutexes to protect shared state.
- `spawn` expects a parameterless procedure. Pass data through globals or pre‑initialized structures.
- Recursive mutexes are useful when helpers invoked under a lock need to acquire the same mutex.
- A typical pattern is: create → lock/unlock as needed → destroy.

Extended helpers (procedure pointers)

In addition to `spawn`/`join`, the following helpers support passing data to a new thread:

- `CreateThread(@Proc, argPtr: Pointer = nil): Thread` – spawn a new thread that calls `Proc`, passing `argPtr` as its first (and only) parameter.
- `WaitForThread(t: Thread): Integer` – wait for completion of the given thread handle (returns 0).

Examples

Spawn and join:
```pascal
procedure Worker; begin (* do work *) end;
var tid: integer;
begin
  tid := spawn Worker;
  join tid;
end.
```

Mutex for shared counter:
```pascal
var counter, mid: integer;

procedure IncWorker;
var i: integer;
begin
  for i := 1 to 100 do begin
    lock(mid);
    counter := counter + 1;
    unlock(mid);
  end;
end;

var t1, t2: integer;
begin
  counter := 0;
  mid := mutex();
  t1 := spawn IncWorker; t2 := spawn IncWorker;
  join t1; join t2;
  destroy(mid);
  WriteLn(counter);  { expected 200 }
end.
```

### **Example Code**

Here is a simple "Hello, World!" program to demonstrate the language's syntax:

```pascal
program HelloWorld;

begin
  WriteLn('Hello, World!');
end.
```
Address-of (`@`)

The `@` operator yields the address of a procedure or function identifier as a pointer value. Example:

```pascal
procedure Handler; begin end;
var p: pointer;
begin
  p := @Handler;
end.
```

Currently `@` is intended for taking routine addresses. Using `@` with non-routine identifiers is not supported.

Procedure and function pointers

Declare procedure/function pointer types using `procedure (...)` or `function (...): <Type>` in a type definition. Assign with `@Name` and call indirectly:

```pascal
type
  PProc = procedure();
  PInc  = function(x: Integer): Integer;

procedure Hello; begin writeln('hi'); end;
function Inc1(x: Integer): Integer; begin Inc1 := x + 1; end;

var p: PProc; f: PInc; n: Integer;
begin
  p := @Hello; p();  // or just p; for parameterless
  f := @Inc1; n := f(41); writeln(n);
end.
```

Assignments and parameter passing perform signature checks (arity and simple parameter/return types).

See also
- A compact, runnable demo combining procedure/function pointers (including indirect calls) with the new thread helpers is available at `Examples/pascal/base/ThreadsProcPtrDemo`.
