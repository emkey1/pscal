# Pscal

Pscal started out as a Pascal interpreter, written for the most part with the help of various AI's.  Most notably Google's Gemini 2.5 Pro and more recently OpenAI's GPT5 in conjunction with their codex.  It has quickly evolved into a VM with multiple front ends, documented below.

The Pascal front end implements a significant subset of classic Pascal.  The code base is written in C and consists of a hand‑written lexer and parser, a bytecode compiler and a stack‑based virtual machine.  Object‑oriented extensions are intentionally avoided.

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

Binaries are written to `build/bin` (e.g. `pscal` and `dscal`).

The `dscal` binary has very verbose debugging enabled

To build without SDL explicitly:

```sh
cmake -DSDL=OFF ..
```

## Tests

After building, run the regression suite:

```sh
ctest --output-on-failure
```

This executes `Tests/run_tests.sh` and exercises both positive and expected failure cases.

## tiny language front end

A minimal compiler for a small educational language, often called *tiny*, is
provided in `tools/tiny`.  It reads source code that follows the grammar
described in the project documentation and emits bytecode that can be executed
by the Pscal virtual machine.

Example usage:

```sh
python tools/tiny program.tiny out.pbc
./build/bin/pscalvm out.pbc
```

Only integer variables and arithmetic are supported, but this is sufficient for
basic experiments or teaching purposes. Example programs demonstrating the
language can be found in `Examples/Clike`.

## CLike front end

`build/bin/clike` implements a compact C-like compiler that integrates
with the pscal vm.  The grammar covers variable and function declarations,
conditionals, loops and expressions. VM builtins can be invoked simply by
calling a function name that lacks a user definition.

Example usage:

```sh
build/bin/clike program.c

```

Sample programs demonstrating the C like fronte end are available in
`Examples/clike`. For a step-by-step guide see
[Docs/clike_tutorial.md](Docs/clike_tutorial.md).

An interactive session is also available via `build/bin/clike-repl`, which
reads a single line of C-like code, wraps it in `int main() { ... }`, and
executes it immediately. For details see
[Docs/clike_repl_tutorial.md](Docs/clike_repl_tutorial.md).

## Runtime library

The interpreter expects access to the `etc` and `lib` directories.  When running from the repository you can create temporary symlinks:

```sh
REPO_DIR="$(pwd)"
sudo mkdir -p /usr/local/Pscal
sudo ln -s "${REPO_DIR}/etc" /usr/local/Pscal/etc
sudo ln -s "${REPO_DIR}/lib" /usr/local/Pscal/lib
```

Alternatively, set the `PSCAL_LIB_DIR` environment variable to point at a copy of the `lib` directory.

## Extending built-ins

Additional VM primitives can be linked in by dropping C source files into
`src/ext_builtins`.  Each file should implement a `registerExtendedBuiltins`
function that registers its routines.  See
[Docs/extending_builtins.md](Docs/extending_builtins.md) for details and an
example that exposes the host process ID in `src/ext_builtins/getpid.c`.

## License

Pscal is released into the public domain under [The Unlicense](LICENSE).

