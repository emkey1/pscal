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

## Compiler options

- `--dump-bytecode`: compile and disassemble bytecode before execution.
- `--dump-bytecode-only`: compile and disassemble bytecode and exit without executing.

These are useful for CI or debugging compiled output.

## Operator semantics

- Logical `&&` and `||` short-circuit like C.
- Shift operators `<<` and `>>` are supported with standard precedence (lower than `+`/`-`, left associative).
- `~x` on integer-typed operands behaves like bitwise NOT; on non-integer types it falls back to logical NOT.

## SDL availability

When building with `-DSDL=ON`, the CLike preprocessor defines `SDL_ENABLED`. You can guard SDL-dependent code like:

```c
#ifdef SDL_ENABLED
  // graphics/audio code here
#else
  printf("SDL not available\n");
#endif
```

For headless environments, set `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy` or leave defaults as provided by the test scripts.

## Imports and library path

CLike supports simple module imports:

```c
import "math_utils.cl";
```

The compiler searches in the current directory, then in `CLIKE_LIB_DIR` if set, and finally in the built-in default install path. For project-local modules, set:

```sh
export CLIKE_LIB_DIR=$(pwd)/Examples/clike
```

## Environment variables

- `CLIKE_LIB_DIR`: directory for imported `.cl` modules.
- `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`: set to `dummy` for headless runs. Set `RUN_SDL=1` to run SDL content.

## HTTP networking (sync)

The CLike front end can call VM HTTP builtins directly. Common helpers:

- `httpsession(): int` – create a session; returns session id or -1 on error.
- `httpclose(s)` – free session.
- `httpsetheader(s, name, value)` – set request header.
- `httpclearheaders(s)` – clear accumulated headers.
- `httpsetoption(s, key, value)` – set options; supported keys include:
  - `timeout_ms` (int), `follow_redirects` (int 0/1), `user_agent` (string)
  - `ca_path`, `client_cert`, `client_key`, `proxy` (strings)
  - `verify_peer` (0/1), `verify_host` (0/1), `http2` (0/1)
  - `basic_auth` (string `user:pass`), `out_file` (string path)
- `httprequest(s, method, url, bodyStr|mstream|null, outMStream): int` – perform request.
- `httprequesttofile(s, method, url, body, outPath): int` – stream response to file.
- `httpgetlastheaders(s): string` – raw response headers for last request.
- `httpgetheader(s, name): string` – convenience header lookup.
- `httperrorcode(s): int` – VM error class (0 none; 1 generic; 2 I/O; 3 timeout; 4 SSL; 5 resolve; 6 connect).
- `httplasterror(s): string` – last libcurl error message if any.

Notes:
- `file://` URLs are supported without libcurl; the runtime reads the file and synthesizes basic headers.
- If `out_file` is set via `httpsetoption`, `httprequest` tees the response to both memory and the file.

Example:

```c
int main() {
  int s = httpsession();
  httpsetheader(s, "Accept", "text/html");
  httpsetoption(s, "timeout_ms", 5000);
  mstream out = mstreamcreate();
  int code = httprequest(s, "GET", "https://example.com", NULL, out);
  printf("status=%d, ctype=%s\n", code, httpgetheader(s, "Content-Type"));
  mstreamfree(&out);
  httpclose(s);
  return 0;
}
```
