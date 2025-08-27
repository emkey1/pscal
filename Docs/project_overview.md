# Pscal Project Overview

Pscal is an extensible virtual machine and compiler suite implemented in C. The project ships with multiple frontends that all target the shared, stack-based virtual machine:

* **Pascal compiler**: A frontend for a Pascal-like language with a hand-written lexer and parser.
* **Clike compiler**: A compact, C-style language frontend that includes its own preprocessor and a REPL for interactive sessions.
* **Tiny compiler**: An educational frontend written in Python.

Detailed descriptions of the Pascal and C-like front ends can be found in
[`pascal_overview.md`](pascal_overview.md) and
[`clike_overview.md`](clike_overview.md). The virtual machine is covered in
[`pscal_vm_overview.md`](pscal_vm_overview.md), and instructions for building
custom front ends or extending VM builtins are in
[`standalone_vm_frontends.md`](standalone_vm_frontends.md) and
[`extending_builtins.md`](extending_builtins.md).

All frontends generate a compact bytecode stream that is executed by the VM. This virtual machine provides a rich set of built-in routines and offers optional integrations with **SDL2** for graphics and audio, and **libcurl** for networking. The system is designed to be easily extensible, allowing for the addition of new built-in functions.

---

## Core Architecture

The project follows a classic compiler and virtual machine design:

* **Frontends**: Each frontend is responsible for parsing its respective language (Pascal-like or C-like) and constructing an Abstract Syntax Tree (AST).
* **Compiler**: A compiler processes the AST, performs semantic analysis (for the C-like language), and generates bytecode. The C-like frontend also includes an optimization pass to improve the generated code.
* **Symbol Table**: A hash table-based symbol table manages variables, functions, and types during compilation.
* **Virtual Machine (VM)**: A stack-based VM executes the bytecode, providing a portable runtime environment.

---

## Key Features and Capabilities

* **Dual Language Support**: The ability to compile both Pascal-like and C-like code to the same bytecode is a major feature.
* **Rich Type System**: Signed and unsigned integers from 8 to 64 bits and floating-point types up to extended precision.
* **Graphics and Audio**: Through SDL bindings, the language supports creating graphical applications with audio capabilities, including window creation, shape and text rendering, and sound playback.
* **Networking**: A networking API using `libcurl` allows for making HTTP requests and handling responses.
* **Rich Built-in Library**: A comprehensive set of built-in functions is provided for:
    * File I/O (`readln`, `writeln`, `fileexists`).
    * Math (`sin`, `cos`, `sqrt`, `factorial`, `fibonacci`, `chudnovsky`).
    * String manipulation (`copy`, `pos`, `length`).
    * System interaction (`getpid`, `dos_exec`).
* **Bytecode Caching**: To speed up subsequent runs, the compiler can cache bytecode for source files that have not been modified.

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
* **Optional**: SDL2, SDL2\_image, SDL2\_mixer and SDL2\_ttf when building with `-DSDL=ON`

---

## Building

```sh
mkdir build && cd build
cmake ..          # add -DSDL=ON to enable SDL support
make
```

To explicitly disable SDL support:

```sh
cmake -DSDL=OFF ..
```

---

## Testing

After compiling, run the regression suite:

```sh
cd Tests; ./run_all_tests
```

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
Pscal is released under The Unlicense.
