# Pscal

Pscal started out as a Pascal interpreter, written for the most part with the help of various AI's.  Most notably Google's Gemini 2.5 Pro and more recently OpenAI's GPT5 in conjunction with their codex.  It has quickly evolved into a VM with multiple front ends, documented below.

The Pascal front end implements a significant subset of classic Pascal.  The code base is written in C and consists of a hand‑written lexer and parser, a bytecode compiler and a stack‑based virtual machine.  Object‑oriented extensions are intentionally avoided, both in the Pascal front end and the pscal code base.

Optional SDL2 support adds graphics and audio capabilities, and built‑in networking routines use libcurl.

## Requirements

- C compiler with C11 support
- [CMake](https://cmake.org/) 3.24 or newer
- [libcurl](https://curl.se/libcurl/)
- **Optional**: SDL2, SDL2_image, SDL2_mixer and SDL2_ttf when building with `-DSDL=ON`

On Debian/Ubuntu the required packages can be installed with:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake libcurl4-openssl-dev \
    libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-ttf-dev
```

## Building

```sh
git clone <repository>
cd pscal
mkdir build && cd build
cmake ..            # add -DSDL=ON to enable SDL support
make
```

Binaries are written to `build/bin` (e.g. `pascal`).
To also build the debugging-oriented `dascal` binary, configure CMake with `-DBUILD_DASCAL=ON`.

The `dascal` binary has very verbose debugging enabled and is not built by default.

To build without SDL explicitly:

```sh
cmake -DSDL=OFF ..
```

## Tests

After building, run the regression suite:

```sh
cd Tests;./run_all_tests
```

- Headless defaults: when SDL is enabled at build time, the test runners default to dummy SDL drivers in headless/CI environments to avoid GUI requirements and noise. SDL-dependent tests are skipped in this mode.
- Force SDL tests: to exercise windowed graphics and input, run with a real video/audio driver and set `RUN_SDL=1`:

  - macOS example (Terminal app in a logged-in GUI session):
    ```sh
    RUN_SDL=1 SDL_VIDEODRIVER=cocoa SDL_AUDIODRIVER=coreaudio ./run_all_tests
    ```
  - Linux example (X11):
    ```sh
    RUN_SDL=1 SDL_VIDEODRIVER=x11 ./run_all_tests
    ```

  If `RUN_SDL=1` is not set, the scripts may export `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy` and skip SDL-specific tests to remain deterministic in CI.

Note: On macOS, you may see benign LaunchServices/XPC warnings on stderr when running SDL tests in some environments.

## Tiny language front end (Written in Python)

A minimal compiler for a small educational language, often called *tiny*, is
provided in `tools/tiny`.  It is written in Python and provides an example of 
how to add a custom stand alone front end that can generate byte code that the
pscal VM/back end can execute.  It compiles source code that follows the grammar
described in the project documentation and emits bytecode that can be executed
by the virtual machine.

Example usage:

```sh
python tools/tiny program.tiny out.pbc
./build/bin/pscalvm out.pbc
```

Only integer variables and arithmetic are supported, but this is sufficient for
basic experiments or teaching purposes. Example programs demonstrating the
language can be found in `Examples/tiny`.

## CLike front end

`build/bin/clike` implements a compact C-like byte code compiler that integrates
with the pscal vm.  The grammar covers variable and function declarations,
conditionals, loops and expressions. VM builtins can be invoked simply by
calling a function name that lacks a user definition.

Example usage:

```sh
build/bin/clike program.cl

```

Sample programs demonstrating the C like front end are available in
`Examples/clike`. For a step-by-step guide see
[Docs/clike_tutorial.md](Docs/clike_tutorial.md).

Options and semantics:

- Command-line options:
  - `--dump-bytecode`: compile and disassemble bytecode (then execute).
  - `--dump-bytecode-only`: compile and disassemble bytecode, then exit (no execution).
- Operator semantics:
  - Logical `&&` and `||` use short-circuit evaluation.
  - Shift operators `<<` and `>>` are supported with standard precedence (lower than `+`/`-`, left-associative).
  - `~x` on integer types behaves like bitwise NOT; on non-integers it falls back to logical NOT.
- SDL feature detection:
  - When built with `-DSDL=ON`, the CLike preprocessor defines `SDL_ENABLED` so you can guard code with `#ifdef SDL_ENABLED`.

An interactive session is also available via `build/bin/clike-repl`, which
reads a single line of C-like code, wraps it in `int main() { ... }`, and
executes it immediately. For details see
[Docs/clike_repl_tutorial.md](Docs/clike_repl_tutorial.md).

## Runtime library

The front ends need access to various sounds and libraries.  To install them run...

```sh
sudo ./install.sh
```

## Extending built-ins

Additional VM builtin functions can be linked in by dropping C source files into
`src/ext_builtins`.  Each file should implement a `registerExtendedBuiltins`
function that registers its routines.  See
[Docs/extended_builtins.md](Docs/extended_builtins.md) for details and an
example that exposes the host process ID in `src/ext_builtins/getpid.c`.

## License

As the Pscal code base was primarily generated by AI, it is released into the public 
domain under [The Unlicense](LICENSE).
