# Tutorial: Using the clike Compiler

The `clike` binary compiles a C-style language and immediately executes it using the PSCAL virtual machine. Source files may omit extensions; many examples in this project do so.

## Build the compiler

From the repository root run:

```sh
cmake -B build
cmake --build build --target clike
```

This generates `build/bin/clike`.

## Install PSCAL Suite
```sh
sudo ./install.sh
```

## Run a program

Invoke the compiler with a source file:

```sh
build/bin/clike path/to/program
```

For example, run the text-based Hangman game:

```sh
build/bin/clike Examples/clike/hangman5
```

The compiler translates the source to VM bytecode and executes it immediately.

## Sample programs

Additional examples live in `Examples/clike`, including `sdl_multibouncingballs.cl` for an SDL demo and `hangman5` for a console game.

