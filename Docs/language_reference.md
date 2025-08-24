# Pscal Language Reference

Pscal is a VM backend with multiple front ends:

This reference first details the Pascal front end and then outlines the CLike and
Tiny front ends:

- **Pascal** – the default front end described below.
- **Clike** – a compact C-like language; see [CLike_overview.md](CLike_overview.md) for
  additional information.
- **Tiny** – an educational toy language implemented in Python located in
  `tools/tiny`.

The Pascal front end implements a substantial subset of classic Pascal with a few
extensions for modern convenience.  It aims to be largely compatible with
traditional Pascal while retaining a small and understandable core.

## Program Structure

A program typically has the following shape:

```pascal
program Hello;

uses crt;      { optional units }

const
  Message = 'Hello, world!';

var
  i: integer;

begin
  for i := 1 to 5 do
    writeln(Message, ' ', i);
end.
```

The `program` heading is optional.  Declarations for constants, types,
variables and subroutines appear before the main `begin ... end.` block.

## Lexical Elements

- **Identifiers** are case‑insensitive and must begin with a letter or
  underscore followed by letters, digits or underscores.
- **Comments** may be written using `{ ... }`, `(* ... *)` or `//` to the
  end of the line.
- **String literals** are enclosed in single quotes; use two single quotes
  to represent an embedded quote: `'don''t'`. C-style escapes such as
  `\n`, `\t`, `\\`, `\e` and `\a` are recognised inside strings.
- **Character literals** are strings of length one or the `#nn` escape for
  code points.

## Basic Types

Pscal provides several simple and structured types:

- `integer` – signed whole numbers.
- `real` – double precision floating point values.
- `boolean` – logical `true` or `false`.
- `char` – single 8‑bit character.
- `string` – sequence of characters with automatic length management.
- **enumerations** – named sets of ordinal values: `type Color = (Red, Green, Blue);`.
- **subranges** – restricted portion of an ordinal type: `type Day = 1..31;`.
- **sets** – collections of small ordinal values: `set of 0..31` or
  `set of Color`.
- **arrays** – fixed‑length sequences: `array[1..10] of integer`.
- **records** – aggregates of fields: `record x, y: integer; end`.
- **pointers** – references to dynamically allocated data: `^TNode`.

### Numeric limits

Integers are represented as signed 64‑bit values. Arithmetic that
produces a result outside the range `-2^63`..`2^63-1` causes the virtual
machine to raise a runtime "Integer overflow" error. As a consequence,
computations that grow rapidly—such as factorials beyond 20!—will halt
once the limit is exceeded. `real` values follow the precision and range
of IEEE‑754 double precision.

## Variables and Constants

- Constants are introduced with the `const` keyword and are immutable:
  `const Pi = 3.1415;`.
- Variables use the `var` keyword and may be global or local to a routine.
  Multiple variables of the same type can be declared together: `var x, y: real;`.
- Variables have default initial values of zero, empty string or `false`.
  Pointer variables default to `nil`.

## Expressions and Operators

Pscal expressions follow standard Pascal precedence rules.  Supported
operators include:

- **Arithmetic** – `+`, `-`, `*`, `/`, `div`, `mod`.
- **Relational** – `=`, `<>`, `<`, `<=`, `>`, `>=`.
- **Logical** – `not`, `and`, `or`, `xor`.
- **Set operators** – `+` (union), `-` (difference), `*` (intersection),
  `in` (membership).
- **Pointer** – `@` obtains the address of a variable; `^` dereferences a
  pointer.
- **Assignment** – `:=` assigns the value on the right to the variable on
  the left.

## Control Flow

Structured statements provide program flow control:

- `if <expr> then <stmt> else <stmt>`
- `case <expr> of ... end`
- `for <var> := <start> to <end> do <stmt>` (or `downto`)
- `while <expr> do <stmt>`
- `repeat ... until <expr>`
- `break` exits the innermost loop.

Labels and `goto` are not supported, keeping the language structured.

## Procedures and Functions

Subroutines encapsulate reusable code.  A routine may be declared before it
is used or nested inside another routine.

- `procedure` – performs an action without returning a value.
- `function` – returns a value by assigning to the function name or using
  `exit(<value>)`.
- Parameters are passed by value unless prefixed with `var` for
  pass‑by‑reference.
- Open array parameters allow routines to accept arrays of any length:
  `procedure Process(var a: array of integer);`.
- Routines may declare local variables and further nested routines which
  can access variables from surrounding scopes.

### Example

```pascal
function Factorial(n: integer): integer;
begin
  if n <= 1 then
    Factorial := 1
  else
    Factorial := n * Factorial(n - 1);
end;

procedure Demo;
var
  i: integer;
begin
  for i := 1 to 5 do
    writeln(i, '! = ', Factorial(i));
end;
```

## Units and the `uses` Clause

Pscal supports modular compilation through units.  A unit file uses the
following structure:

```pascal
unit MathHelpers;

interface

function Max(a, b: integer): integer;

implementation

function Max(a, b: integer): integer;
begin
  if a > b then Max := a else Max := b;
end;

end.
```

A program imports unit definitions with the `uses` clause.  Units may
declare types, variables and routines in their interface section.  Code in
the implementation section is private to the unit.

## Built‑in Procedures and Functions

The runtime provides many useful routines for text I/O, mathematics and
system interaction.  See [Pscal_Builtins](Pscal_Builtins.md) for the
complete list of available built‑ins.

## CLike Front End

The CLike front end provides a compact C-style language that targets the Pscal
virtual machine.

### Program Structure

Programs consist of functions; execution begins at `int main()`. Blocks are
delimited by braces `{}` and statements end with semicolons.

### Lexical Elements

- Identifiers are case-sensitive and may contain letters, digits and
  underscores.
- `//` introduces a single-line comment and `/* ... */` delimits block
  comments.
- String literals use double quotes and recognise standard C escape sequences.

### Types

CLike offers `int` for 64-bit integers, `double` for floating point values and
`str` for mutable strings. Arrays are zero-based while string indexing starts at
1 (`s[1]` is the first character). A `str` variable already evaluates to a
pointer to its buffer.

### Expressions and Operators

Operators follow C conventions with arithmetic (`+`, `-`, `*`, `/`), comparison
(`==`, `!=`, `<`, `<=`, `>`, `>=`), logical (`&&`, `||`, `!`) and assignment
(`=`) forms. `+` concatenates strings. Pointers use the `->` operator and
`new(&node);` allocates structures on the heap.

### Control Flow

The usual C statements are available: `if`, `while`, `for`, `do...while` and
`break` to exit a loop.

### Example

```clike
void sort_string(str s) {
    int i, j, len;
    char tmp;
    len = strlen(s);
    i = 1;
    while (i <= len) {
        j = i + 1;
        while (j <= len) {
            if (s[i] > s[j]) {
                tmp = s[i];
                s[i] = s[j];
                s[j] = tmp;
            }
            j++;
        }
        i++;
    }
}
```

Built-in routines such as `strlen`, `copy` and `upcase` operate on `str` values
and `new(&node);` performs dynamic allocation.

## Tiny Front End

Tiny is a minimal educational language implemented in Python. It compiles to
Pscal bytecode and supports only integer variables and arithmetic.

### Program Structure

Programs are simple sequences of statements. Variables are implicitly created on
first use and hold 64-bit integers.

### Lexical Elements

- Comments are enclosed in `{` and `}`.
- Statements are terminated by `;` and assignments use `:=`.

### Control Flow

- `if <expr> then <stmt> else <stmt> end;`
- `repeat <stmts> until <expr>;`

### Input and Output

`read` retrieves an integer from standard input and `write` outputs a value or
blank line.

### Example

```tiny
{ Sum numbers from 1 to n }
read n;
sum := 0;
repeat
    sum := sum + n;
    n := n - 1
until n = 0;
write sum
```


