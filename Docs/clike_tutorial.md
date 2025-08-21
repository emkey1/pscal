# Tutorial: Using the clike Compiler

The `clike` binary compiles a small C-style language and immediately executes it on the Pscal virtual machine.

## Build the compiler

From the repository root run:

```sh
cmake -B build
cmake --build build --target clike
```

This generates `build/bin/clike`.

## Run a program

Invoke the compiler with a source file:

```sh
build/bin/clike path/to/program.cl
```

For example, run the text-based Hangman game:

```sh
build/bin/clike Examples/Clike/hangman5.cl
```

The compiler translates the source to VM bytecode and executes it immediately.

## Sample programs

Additional examples live in `Examples/Clike`, including `sdl_multibouncingballs.cl` for an SDL demo and `hangman5.cl` for a console game.

