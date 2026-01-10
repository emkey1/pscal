# PSCAL Project Overview

PSCAL is an extensible virtual machine and compiler suite implemented in C. The project ships with multiple frontends that all target the shared, stack-based virtual machine:

* **Pascal compiler**: A frontend for a Pascal-like language with a hand-written lexer and parser.
* **Clike compiler**: A compact, C-style language frontend that includes its own preprocessor and a REPL for interactive sessions.
* **Rea compiler**: An object-oriented language with classes, inheritance, and closures.
* **exsh shell**: A Bash-compatible shell frontend that orchestrates processes and VM builtins.
* **Tiny compiler**: An educational frontend written in Python.

Detailed descriptions of the Pascal, C-like, Rea, and exsh front ends can be
found in [`pascal_overview.md`](pascal_overview.md),
[`clike_overview.md`](clike_overview.md), [`rea_overview.md`](rea_overview.md),
and [`exsh_overview.md`](exsh_overview.md). The virtual machine is covered in
[`pscal_vm_overview.md`](pscal_vm_overview.md), and instructions for building
custom front ends or extending VM builtins are in
[`standalone_vm_frontends.md`](standalone_vm_frontends.md) and
[`extended_builtins.md`](extended_builtins.md).

All frontends generate a compact bytecode stream that is executed by the VM. This virtual machine provides a rich set of built-in routines and offers optional integrations with **SDL2/SDL3** for graphics and audio, and **libcurl** for networking. The system is designed to be easily extensible, allowing for the addition of new built-in functions.

---

## Core Architecture

The project follows a classic compiler and virtual machine design:

* **Frontends**: Each frontend is responsible for parsing its respective language (Pascal, C-like, Rea, exsh scripts, or Tiny) and constructing an Abstract Syntax Tree (or shell command graph).
* **Compiler**: A compiler processes the AST, performs semantic analysis (for the C-like language), and generates bytecode. The C-like frontend also includes an optimization pass to improve the generated code.
* **Symbol Table**: A hash table-based symbol table manages variables, functions, and types during compilation.
* **Virtual Machine (VM)**: A stack-based VM executes the bytecode, providing a portable runtime environment.

---

## Key Features and Capabilities

* **Multiple Front Ends**: Pascal, CLike, Rea, exsh, and the Tiny reference compiler all target the same VM bytecode so teams can pick the syntax that fits their workflow without losing runtime parity.
* **Rich Type System**: Signed and unsigned integers from 8 to 64 bits and floating-point types up to extended precision.
* **Graphics and Audio**: Through SDL bindings, the language supports creating graphical applications with audio capabilities, including window creation, shape and text rendering, and sound playback.
* **Networking**: HTTP helpers via `libcurl` plus low-level TCP/UDP sockets with DNS utilities.
* **Rich Built-in Library**: A comprehensive set of built-in functions is provided for:
    * File I/O (`readln`, `writeln`, `fileexists`).
    * Math (`sin`, `cos`, `sqrt`, `factorial`, `fibonacci`, `chudnovsky`).
    * String manipulation (`copy`, `pos`, `length`).
    * System interaction (`getpid`, `dosExec`).
* **Multithreading**: Lightweight threads can be created with `spawn`/`join`, the higher-level `CreateThread` helpers, or the worker-pool builtins (`ThreadSpawnBuiltin`, `ThreadPoolSubmit`, `ThreadSetName`, `ThreadLookup`, `ThreadGetResult`, `ThreadGetStatus`, `ThreadPause`, `ThreadResume`, `ThreadCancel`, `ThreadStats`) for asynchronous builtins and background jobs.
* **Bytecode Caching**: To speed up subsequent runs, the compiler can cache bytecode for source files that have not been modified. Cached bytecode carries a version tag; programs can query `VMVersion` and `BytecodeVersion` to decide how to handle mismatches. Set `PSCAL_STRICT_VM=1` to have the VM abort when bytecode targets a newer VM.
  Example programs demonstrating these builtins are `Examples/pascal/base/VMVersionDemo`
  and `Examples/clike/base/vm_version_demo`.
* **Optimised Runtime Dispatch**: Builtin lookups now go through a hash-indexed registry
  and the VM caches procedure symbols by bytecode address, so call overhead stays flat
  even as new builtins and modules are added. The cache is refreshed automatically when
  the interpreter loads fresh bytecode.

---

## How It Works

1.  **Parsing**: Source code is processed by the appropriate lexer and parser to build an Abstract Syntax Tree (AST).
2.  **Semantic Analysis**: For the C-like language, a semantic analysis pass checks the AST for correctness, such as type errors.
3.  **Compilation**: The compiler traverses the AST to generate a portable, low-level bytecode representation of the program.
4.  **Execution**: The virtual machine executes the bytecode. As a stack-based machine, it uses a stack for data manipulation and calculations while processing the bytecode instructions.

---

## Requirements

* C compiler with C11 support
* [CMake](https://cmake.org/) 3.24 or newer
* [libcurl](https://curl.se/libcurl/)
* **Optional**: SDL2 or SDL3 (plus the matching `SDL*_image`, `SDL*_mixer`, and `SDL*_ttf` add-ons) when building with `-DSDL=ON`

---

## Building

```sh
cmake -S . -B build     # add -DSDL=ON to enable SDL support, add -DRELEASE_BUILD=ON for release packaging toggles
cmake --build build
```

To explicitly disable SDL support:

```sh
cmake -S . -B build -DSDL=OFF
cmake --build build
```

---

## Testing

After compiling, run the regression suite:

```sh
./Tests/run_all_tests
```

The harness now auto-selects a writable temporary directory and honours
`RUN_NET_TESTS=1` / `RUN_SDL=1` when you want to exercise network or graphics
suites.

---

## Directory Layout

* src/: Core compiler(s) and virtual machine sources
    * backend_ast: Contains the backend of the compiler, focusing on the Abstract Syntax Tree (AST). It includes the code for built-in functions and interfaces
    * clike: The complete frontend for the C-like language.
    * compiler: Contains core components of the compiler that are shared across the different language frontends such as bytecode definition and the portion that translates the AST into bytecode.
    * core: Houses fundamental utilities and data structures used throughout the project. This includes implementations for lists, bytecode caching, and common type definitions.
    * ext_builtins: The source code for additional, "external" built-in functions. This modular design allows for new features to be added to the language easily.
    * pascal: The Pascal-like language frontend, containing its lexer, parser, and AST implementation.
    * symbol: The implementation of the symbol table, which is a critical component for the compiler to manage identifiers such as variables and functions.
    * vm: The source code for the stack-based virtual machine that executes the bytecode produced by the front ends.
* lib/pascal/: Standard library units written in Pscal
* lib/clike/: Standard modules written in clike
* lib/sounds/: Audio assets shared by front ends
* Examples/: Sample programs for each front end
* Docs/: Project and language documentation
* tools/: Additional front ends and utilities (e.g. tools/tiny)
* Tests/: Regression suite

## License
Pscal is released under the MIT License. PSCAL releases prior to 2.22 were distributed
under the Unlicense.
