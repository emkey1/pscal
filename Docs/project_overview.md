# Pscal Project Overview

Pscal is a Pascal‑like language implemented in C.  The code base contains
three major components:

* **Front end** – hand written lexer and parser.
* **Bytecode compiler** – generates a compact instruction stream.
* **Virtual machine** – executes the bytecode and provides built‑in routines.

Optional SDL2 and libcurl integrations add graphics, audio and basic
networking capabilities.

## Building

Pscal uses CMake.  A typical non‑SDL build looks like:

```sh
mkdir build && cd build
cmake -DSDL=OFF ..
make
```

Enable SDL support by passing `-DSDL=ON` and ensuring the SDL2 development
libraries are installed.

## Testing

After compiling, run the regression suite:

```sh
cd Tests; ./run_all_tests
```

## Directory Layout

* `src/` – compiler and virtual machine sources
* `lib/pascal/` – standard library units written in Pscal
* `lib/clike/` – standard modules written in clike
* `lib/sounds/` – audio assets shared by front ends
* `Examples/` – small sample programs
* `Docs/` – project and language documentation

## License

Pscal is released under [The Unlicense](../LICENSE).

