# Pscal Functionality Overview

## Core Architecture

Pscal follows a traditional compiler pipeline composed of a front end, a bytecode compiler, and a virtual machine.

### Front End
The front end tokenises source text with a hand‑written lexer that recognises Pascal keywords such as `begin`, `record`, `set`, `while` and `writeln`【F:src/Pascal/lexer.c†L10-L33】. The parser builds an abstract syntax tree (AST) for programs, units, and type declarations, supporting constructs like enums and records.

### Bytecode Compiler
The compiler walks the AST and emits bytecode instructions into a `BytecodeChunk`. It maintains scope information for locals, loop handling, and constant pools【F:src/compiler/compiler.c†L1-L72】.

### Virtual Machine
The VM executes bytecode on a stack-based interpreter. It provides helper routines for stack management and debugging and integrates with built‑in procedures through a dispatch table【F:src/vm/vm.c†L1-L78】【F:src/backend_ast/builtin.c†L35-L77】.

## The Language

Pscal implements a substantial subset of classic Pascal:

* **Basic types** – integer, real, boolean, char, string, enumerations, sets and records.
* **Control flow** – `if`, `case`, `for`, `while`, `repeat…until`, and `break`.
* **Subroutines** – functions and procedures with local variables and parameters.
* **Units** – separate compilation modules that export types, variables and routines.

Example program:

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

## Builtins

Builtins are implemented in C and exposed to Pascal through a lookup table【F:src/backend_ast/builtin.c†L35-L176】. They cover:

* **Math** – `abs`, `cos`, `sin`, `tan`, `exp`, `ln`, `sqrt`, `sqr`, `round`, `trunc`.
* **Strings** – `length`, `copy`, `pos`, `chr`, `ord`, `inttostr`, `realtostr`, `upcase`.
* **I/O** – `readln`, `writeln`, `readkey` (optional char parameter), `halt`, `gotoxy`, `whereX`, `textcolor`.
* **Files & streams** – `assign`, `reset`, `rewrite`, `close`, `eof`, memory stream helpers (`mstreamcreate`, `mstreamloadfromfile`, etc.).
* **Random numbers** – `randomize`, `random`.
* **System utilities** – `paramcount`, `paramstr`, `exit`, `ioresult`.
* **DOS compatibility** – directory and environment calls (`dos_findfirst`, `dos_getenv`, ...).

Example:

```pascal
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
Provides simple string helpers such as reversing text【F:lib/StringUtil.pl†L1-L28】.

* `ReverseString(s: string): string` – returns the characters of `s` in reverse order.

Example:
```pascal
uses StringUtil;
begin
  writeln(ReverseString('hello'));  // olleh
end.
```

### Base64
Implements Base64 encoding and decoding【F:lib/base64.pl†L1-L155】.

* `EncodeStringBase64(s: string): string`
* `DecodeStringBase64(s: string): string`

### CalculateArea
Provides geometry helpers【F:lib/calculatearea.pl†L1-L31】.

* `RectangleArea(length, width: real): real`
* `CircleArea(radius: real): real`
* `TriangleArea(side1, side2, side3: real): real` – uses Heron’s formula.

### CRT
ANSI terminal control compatible with Turbo Pascal’s CRT unit【F:lib/crt.pl†L26-L57】.

* Screen and cursor routines: `ClrScr`, `GotoXY`, `ClrEol`, `Window`, `WhereX`, `WhereY`.
* Text attributes: `TextColor`, `TextBackground`, `NormVideo`, `HighVideo`, `LowVideo`, `InvertColors`, `BoldText`, `BlinkText`.
* Miscellaneous: `Delay`, `Beep`, `KeyPressed`, `SaveCursor`, `RestoreCursor`.

### Dos
Provides DOS‑style file and environment operations by wrapping builtins【F:lib/dos.pl†L1-L66】.

* `FindFirst`, `FindNext`
* `GetFAttr`, `MkDir`, `RmDir`
* `GetEnv`
* `GetDate`, `GetTime`
* `Exec`

### MathLib
Advanced mathematical routines【F:lib/mathlib.pl†L1-L136】.

* Trigonometry: `ArcTan`, `ArcSin`, `ArcCos`, `Cotan`
* Exponentials: `Power`, `Log10`, `Sinh`, `Cosh`, `Tanh`
* Helpers: `Max`, `Min`, `Floor`, `Ceil`

### mylib
Example user unit exporting a record type and routines【F:lib/mylib.pl†L1-L40】.

* `Greet(name: string)`
* `Add(a, b: integer): integer`
* `GetPi: real`
* `TPerson` record and `GlobalCounter` variable

### SysUtils
Common utility routines【F:lib/sysutils.pl†L1-L132】.

* `UpperCase`, `LowerCase`, `Trim`, `QuotedStr`
* `FileExists`

Example:
```pascal
uses SysUtils;
begin
  if FileExists('data.txt') then
    writeln(UpperCase('found file'));
end.
```

## Summary
Pscal offers a Pascal environment with a modular architecture, a rich set of builtins, and reusable units for console I/O, math, string handling and system access. These tools make it suitable both for educational purposes and for experimenting with Pascal‑like language design.
