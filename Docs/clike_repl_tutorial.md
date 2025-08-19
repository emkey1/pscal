# Tutorial: Using the clike REPL

The `clike-repl` executable lets you experiment interactively with the small C-like language that targets the Pscal virtual machine.

## Build the REPL

From the repository root run:

```sh
cmake -B build
cmake --build build --target clike-repl
```

This creates `build/bin/clike-repl`.

## Start an interactive session

Launch the REPL:

```sh
build/bin/clike-repl
```

Each line you enter is wrapped in a minimal `int main()` function and executed immediately.

## Example usage

```
$ build/bin/clike-repl
clike> println("hello world");
hello world
clike> 2 + 2;
4
clike> :quit
```

Use `:quit` to exit the session.

