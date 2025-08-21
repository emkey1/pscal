# Pscal Project Overview

Pscal is an extensible VM implemented in C. The project ships with
multiple front ends that all target the shared virtual machine:

* **Pascal compiler** – hand‑written lexer and parser.
* **Clike compiler** – compact C‑style language example.
* **Tiny compiler** – educational front end written in Python.

All front ends generate a compact bytecode stream executed by the stack‑based virtual
machine. The VM provides built‑in routines and optional SDL2 and libcurl
integrations for graphics, audio and networking.  It also offers easy integration of 
additional builtins that can be called as functions by the Pascal and CLike front ends.

## Requirements

* C compiler with C11 support
* [CMake](https://cmake.org/) 3.24 or newer
* [libcurl](https://curl.se/libcurl/)
* **Optional**: SDL2, SDL2_image, SDL2_mixer and SDL2_ttf when building with
  `-DSDL=ON`

## Building

```sh
mkdir build && cd build
cmake ..            # add -DSDL=ON to enable SDL support
make
```

To explicitly disable SDL support:

```sh
cmake -DSDL=OFF ..
```

Binaries are placed in `build/bin` (e.g. `pascal`, `clike` and `pscalvm`).

## Testing

After compiling, run the regression suite:

```sh
cd Tests; ./run_all_tests
```

## Directory Layout

* `src/` – core compiler and virtual machine sources
* `lib/pascal/` – standard library units written in Pscal
* `lib/clike/` – standard modules written in clike
* `lib/sounds/` – audio assets shared by front ends
* `Examples/` – sample programs for each front end
* `Docs/` – project and language documentation
* `tools/` – additional front ends and utilities (e.g. `tools/tiny`)
* `Tests/` – regression suite

## License

Pscal is released under [The Unlicense](../LICENSE).

