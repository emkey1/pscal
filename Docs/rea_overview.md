# Rea Front End Overview

### Front End
The experimental Rea front end emits PSCAL AST nodes directly, mirroring the Pascal pipeline so that code can be compiled without a translation layer. It currently handles basic expressions while paving the way for a complete compiler【F:src/rea/README.md†L3-L7】.

Numeric literals default to 64-bit integers or doubles to preserve their full range, and the command line offers diagnostic flags such as `--dump-ast-json`, `--dump-bytecode` and `--dump-bytecode-only` to inspect compilation artifacts【F:src/rea/README.md†L9-L20】.

### Bytecode Compiler
Planned work includes building a dedicated lexer and recursive‑descent parser, performing semantic analysis with a class‑aware symbol table, and generating PSCAL bytecode that supports object allocation, field access and virtual method dispatch. Integrating the compiler into the existing build and test infrastructure rounds out the roadmap【F:src/rea/README.md†L22-L31】.

### Virtual Machine
Rea targets the existing PSCAL virtual machine. Dynamic dispatch will rely on per-class V‑tables so that overridden methods are resolved at runtime, extending the VM's call frame and symbol table as needed【F:src/rea/LANGUAGE_SPEC.md†L76-L83】.

## The Language

Rea is a strongly typed, class‑based language with C‑like tokens. Comments use `//` (or `/* ... */` for multi-line blocks), identifiers are case‑sensitive, and keywords cover data types (`int`, `float`, `str`, `bool`, etc.), class constructs (`class`, `new`, `extends`, `my`, `myself`, `super`), and control flow (`if`, `while`, `for`, `switch`, `break`, `continue`, `return`). Common arithmetic and comparison operators—including integer division via `//`—are supported, and string and character literals follow C conventions with escape sequences【F:src/rea/LANGUAGE_SPEC.md†L14-L37】.

Primitive types map directly onto VM representations, standardizing on 64‑bit integers and doubles by default. Object‑oriented features include classes with fields and methods, constructors invoked via `new`, dot‑based field and method access, `my`/`myself` and `super` references, single inheritance through `extends`, and dynamic dispatch through V‑tables. Method overloading is deliberately omitted; each method and function must have a unique name. The language has access to the VM's built‑in routines for I/O, math, strings, and threading【F:src/rea/LANGUAGE_SPEC.md†L41-L90】.

### Example: Simple Counter
```rea
class Counter {
  int n;
  void inc() { my.n = my.n + 1; }
  int set(int v) { my.n = v; return my.n; }
}

Counter c = new Counter();
c.set(3);
writeln("c=", c.n);
c.inc();
writeln("c=", c.n);
```
Example program demonstrating field assignment and method calls【F:Examples/rea/base/method_demo†L3-L19】.

For additional examples and the full specification, see `rea_language_reference.md` and the sample programs under `Examples/rea/base` and `Examples/rea/sdl`.

### The Name

Rea (pronounced like the English word "Area" without the leading A) was my best friend, wife and near 
constant companion for twenty five plus years.  She passed in January of 2021 from complications related 
to Creutzfeldt-Jakob Disease (CJD), an increadily rare degenerative brain disease that strikes about 
four hundred people a year in the US, roughly one in a million.

If you find this code useful, please consider a donation to the CJD foundation in her name (Rea Simpson).
