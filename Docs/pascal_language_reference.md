# Pascal Language Specification

### **Introduction and Design Philosophy**

This document specifies the Pascal-like language, a front end for the PSCAL virtual machine (VM). The language is designed to offer a classic, Pascal-style syntax for developers, particularly those familiar with Turbo Pascal or Delphi. It compiles down to the same bytecode as the C-like front end, allowing for a choice of syntactic flavors while targeting a single, unified runtime environment.

The language is strongly typed and emphasizes readability and structured programming. It supports a variety of traditional Pascal features, including a robust type system with records, arrays, enums, sets, and pointers, as well as a modular programming system using units.

### **Lexical Structure**

#### **Comments**

* **Brace-delimited comments:** Start with `{` and end with `}`. These can be nested.
* **Parenthesis-star comments:** Start with `(*` and end with `*)`. These can also be nested.
* **Single-line comments:** Start with `//` and extend to the end of the line.

#### **Keywords**

The following are reserved keywords and cannot be used as identifiers:

* `and`, `array`, `begin`, `break`
* `case`, `const`, `div`, `do`, `downto`
* `else`, `end`, `enum`
* `false`, `for`, `function`
* `if`, `implementation`, `in`, `inline`, `initialization`, `interface`, `join`
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
| `MStream` | `TYPE_MEMORYSTREAM` | A dynamic in-memory stream of bytes. |

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
* **`array`:**
    ```pascal
    type
      MyArray = array[1..10] of real;
    ```
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

The Pascal front end has access to the full suite of built-in routines provided by the PSCAL VM. This includes extensive support for:
* File I/O (`Assign`, `Reset`, `Rewrite`, `Read`, `Write`, `Close`, etc.)
* String manipulation (`Length`, `Copy`, `Pos`, `Concat`, etc.)
* Mathematical functions (`Sin`, `Cos`, `Sqrt`, `Abs`, etc.)
* Memory management (`New`, `Dispose`)
* Console I/O (`GotoXY`, `TextColor`, `ClrScr`, etc.)
* SDL-based graphics and sound, if compiled with SDL support.

### **Threading**

The Pascal front end supports lightweight concurrency using two keywords:

* `spawn` – starts a new thread executing a parameterless procedure and returns a thread identifier.
* `join` – waits for the thread whose identifier is given to finish execution.

Example:

```pascal
var tid: integer;
begin
  tid := spawn WorkerProc;
  join tid;
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
