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
git clone https://github.com/emkey1/pscal.git
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

- Network tests: to keep CI deterministic, tests that require outbound network are guarded.
  - Pascal examples include a demo (`Examples/Pascal/HttpHeadersNetDemo`) that only runs when `RUN_NET_TESTS=1` is set.
  - The CLike test runner will skip any test with a `.net` sentinel file unless `RUN_NET_TESTS=1` is set.

Note: On macOS, you may see benign LaunchServices/XPC warnings on stderr when running SDL tests in some environments.

## Running the new example (threads + procedure pointers)

Two convenient ways to run the demo that exercises procedure/function pointers (including indirect calls) and the new `CreateThread(@Proc, arg)`/`WaitForThread(t)` APIs:

1) CMake custom target:

```sh
cmake --build build --target run_threads_procptr_demo
```

2) Makefile in `Examples/`:

```sh
make -C Examples threads-procptr-demo
```

The example source lives at `Examples/Pascal/ThreadsProcPtrDemo`.

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
./build/bin/pscald out.pbc
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

### Simple Web Server (CLike)

A minimal HTTP server written in CLike is available at
`Examples/clike/simple_web_server`. It serves files from a specified directory,
with `index.html` auto-detected for `/`, and includes a small worker pool and
metrics.

- Quick start (default port 5555, ephemeral root):
  ```sh
  build/bin/clike Examples/clike/simple_web_server
  ```
- Serve an existing directory on port 8080:
  ```sh
  build/bin/clike Examples/clike/simple_web_server 8080 /path/to/htdocs
  # or
  build/bin/clike Examples/clike/simple_web_server /path/to/htdocs 8080
  ```
- Optional tuning (threads, queue cap):
  ```sh
  build/bin/clike Examples/clike/simple_web_server /path 8080 16 128
  ```

Documentation: see [Docs/simple_web_server.md](Docs/simple_web_server.md).

Tip: a basic `htdocs` directory for this server lives in the PSCAL clone under
`lib/misc/simple_web_server/htdocs`.

Options and semantics:

- Command-line options:
  - `--dump-bytecode`: compile and disassemble bytecode (then execute).
  - `--dump-bytecode-only`: compile and disassemble bytecode, then exit (no execution).
- Standalone tools:
  - `pscald <bytecode_file>`: disassemble a compiled bytecode file. Output matches `--dump-bytecode-only`.
- Operator semantics:
  - Logical `&&` and `||` use short-circuit evaluation.
  - Shift operators `<<` and `>>` are supported with standard precedence (lower than `+`/`-`, left-associative).
  - `~x` on integer types behaves like bitwise NOT; on non-integers it falls back to logical NOT.
- SDL feature detection:
  - When built with `-DSDL=ON`, the CLike preprocessor defines `SDL_ENABLED` so you can guard code with `#ifdef SDL_ENABLED`.

Environment variables:

- `CLIKE_LIB_DIR`: search directory for CLike `import "..."` modules.
- `PASCAL_LIB_DIR`: root directory for Pascal units (`.pl` files). The test runner stages a copy under this path.
- `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`: set to `dummy` by default in headless runs; set `RUN_SDL=1` to execute SDL examples/tests.
- `RUN_NET_TESTS`: when set to `1`, enables network-dependent tests and demos.

### HTTP networking (sync)

Built-in HTTP helpers are available to all front ends (Pascal and CLike). Highlights:

- Sessions: `HttpSession/httpsession`, `HttpClose/httpclose`.
- Headers: `HttpSetHeader/httpsetheader`, `HttpClearHeaders/httpclearheaders`, `HttpGetLastHeaders/httpgetlastheaders`, `HttpGetHeader/httpgetheader`.
- Options via `HttpSetOption/httpsetoption` (key → value):
  - `timeout_ms`, `follow_redirects`, `user_agent`
  - Compression: `accept_encoding` (e.g., `gzip` or empty string for all supported encodings)
  - TLS: `ca_path`, `client_cert`, `client_key`, hostname checks via `verify_peer`, `verify_host`
  - Proxy: `proxy`
  - HTTP/2: `http2`
  - Auth: `basic_auth` (`user:pass`)
  - Output: `out_file` (tee response to file in `HttpRequest`)
- Requests:
  - Memory: `HttpRequest/httprequest(s, method, url, bodyStr|mstream|nil, outMStream)` → status code
  - File: `HttpRequestToFile/httprequesttofile(s, method, url, body, outPath)` → status code
- Errors: `HttpErrorCode/httperrorcode` (0 none; 1 generic; 2 I/O; 3 timeout; 4 SSL; 5 resolve; 6 connect), `HttpLastError/httplasterror` message.

Notes:
- `file://` URLs are handled directly by the runtime with synthesized `Content-Length` and `Content-Type` headers; this enables hermetic tests without relying on libcurl’s file scheme.

See also: Docs/http_security.md for details on pinning, TLS knobs, proxies, and DNS overrides with step‑by‑step commands.

#### TLS, Security, and Proxies

Configure per-session knobs via `HttpSetOption/httpsetoption`:

- TLS constraints:
  - `tls_min` / `tls_max`: integers 10/11/12/13 map to TLSv1.0/1.1/1.2/1.3 (min and max cap when supported).
  - `alpn`: 0/1 to disable/enable ALPN (when libcurl supports it).
  - `ciphers`: OpenSSL-style cipher list string for `CURLOPT_SSL_CIPHER_LIST`.
  - `pin_sha256`: pinned public key (string). Use `sha256//BASE64` or a file path per libcurl `CURLOPT_PINNEDPUBLICKEY` format.

- Proxies:
  - `proxy`: proxy URL (e.g., `http://host:8080`).
  - `proxy_userpwd`: `user:pass` credentials.
  - `proxy_type`: `http`, `https` (if supported by your libcurl), `socks5`, or `socks4`.

- DNS overrides:
  - `resolve_add`: add an entry `host:port:address` (e.g., `example.com:443:93.184.216.34`).
  - `resolve_clear`: clear all resolve overrides.

## AST JSON → Bytecode (pscaljson2bc)

`pscaljson2bc` compiles an AST JSON stream (as produced by any front end's `--dump-ast-json`) into VM bytecode.

- Input: a file path or `-`/stdin.
- Output: raw bytecode to stdout or `-o <file>`.
- Options:
  - `--dump-bytecode` disassembles the generated bytecode to stderr before writing the raw bytes.
  - `--dump-bytecode-only` disassembles to stderr and exits without writing raw bytecode.

Examples:

```sh
# Compile Pascal source via JSON pipeline to bytecode file
build/bin/pascal --dump-ast-json Tests/Pascal/BoolTest | build/bin/pscaljson2bc -o out.bc

# Or read from a JSON file captured earlier
build/bin/pscaljson2bc --dump-bytecode-only /path/to/ast.json

# Execute generated bytecode
build/bin/pscalvm out.bc
# Disassemble bytecode
build/bin/pscald out.bc
```

Install: `pscaljson2bc` is installed alongside `pascal` and `pscalvm` by `cmake --install <build-dir>`.

More: see the full guide at `Docs/pscaljson2bc.md`.

Shell completions:
- Bash: source `tools/completions/pscaljson2bc.bash` to enable option completion:
  ```sh
  source tools/completions/pscaljson2bc.bash
  ```
- Zsh: add `tools/completions` to your `fpath` or copy `_pscaljson2bc` into a directory in `fpath`, then `autoload -Uz compinit && compinit`.
  ```zsh
  fpath=("$PWD/tools/completions" $fpath)
  autoload -Uz compinit && compinit
  ```
Completions can also be installed automatically by CMake (see the `PSCAL_INSTALL_COMPLETIONS` option).

All of the above apply to both sync and async requests. Async jobs snapshot session options at submission.

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

## Tools

- `tools/pin-from-host.sh`: computes a libcurl-compatible SPKI pin (`sha256//BASE64`) from a live host or PEM file. Useful with `HttpSetOption(s, 'pin_sha256', ...)`. See `Docs/http_security.md` for usage.

## License

As the Pscal code base was primarily generated by AI, it is released into the public 
domain under [The Unlicense](LICENSE).
