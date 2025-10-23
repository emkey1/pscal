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
sudo cmake --install build
```

## Run a program

Invoke the compiler with a source file:

```sh
build/bin/clike path/to/program
```

For example, run the text-based Hangman game:

```sh
build/bin/clike Examples/clike/base/hangman5
```

The compiler translates the source to VM bytecode and executes it immediately.

## Sample programs

Additional examples live in `Examples/clike/base` and `Examples/clike/sdl`, including `sdl/multibouncingballs` for an SDL demo and `base/hangman5` for a console game.

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
export CLIKE_LIB_DIR=$(pwd)/Examples/clike/base
```

## Environment variables

- `CLIKE_LIB_DIR`: directory for imported `.cl` modules.
- `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`: set to `dummy` for headless runs. Set `RUN_SDL=1` to run SDL content.

## Examples

- Simple Web Server (CLike):
  - Source: `Examples/clike/base/simple_web_server`
  - Quick start: `build/bin/clike Examples/clike/base/simple_web_server /path/to/htdocs 8080`
  - Documentation: `Docs/simple_web_server.md`
  - A basic `htdocs` tree is available in the PSCAL clone at `lib/misc/simple_web_server/htdocs`.

## Thread helpers

CLike maps the VM's worker-pool builtins to lowercase helpers:

- `thread_spawn_named(target, name, ...)` – launches an allow-listed builtin on a worker thread, returning a `Thread` handle. Additional arguments are forwarded to the builtin before an options record that sets the thread name.
- `thread_pool_submit(target, name, ...)` – queues work on the shared pool without stealing the worker from the caller. The returned handle can be joined with `WaitForThread`.
- `thread_set_name(handle, name)` / `thread_lookup(nameOrId)` – rename handles or resolve them by name.
- `thread_pause`, `thread_resume`, and `thread_cancel` mirror the VM control operations; the helpers return `1` on success and `0` otherwise.
- `thread_get_result(handle, consumeStatus)` and `thread_get_status(handle, dropResult)` surface the stored payloads and success flags. Pass a non-zero second argument to release the slot after reading.
- `thread_stats()` returns an array of records describing each worker participating in the pool so scripts can report metrics or feed dashboards.

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

### TLS/Security and Proxies

Per-session options configured with `httpsetoption`:

- TLS:
  - `tls_min` / `tls_max`: 10, 11, 12, 13 map to TLSv1.0–TLSv1.3 (min and max).
  - `alpn`: 0/1 toggles ALPN when available.
  - `ciphers`: set cipher list (OpenSSL format).
  - `pin_sha256`: pinned public key for `CURLOPT_PINNEDPUBLICKEY` (`sha256//BASE64` or file path).
- Proxies:
  - `proxy`: proxy URL (e.g., `http://proxy:8080`).
  - `proxy_userpwd`: `user:pass` credentials.
  - `proxy_type`: `http`, `https` (if supported), `socks5`, `socks4`.
- DNS overrides:
  - `resolve_add`: add `host:port:address` mapping.
  - `resolve_clear`: clear overrides.

Example (guarded by RUN_NET_TESTS):

```c
int main() {
  if (getenv("RUN_NET_TESTS") == NULL || strcmp(getenv("RUN_NET_TESTS"), "1") != 0) {
    printf("Set RUN_NET_TESTS=1 to run this demo.\n");
    return 0;
  }
  const char* url = getenv("URL"); if (!url) url = "https://example.com";
  const char* pin = getenv("PIN_SHA256");
  int s = httpsession();
  httpsetoption(s, "tls_min", 12);
  httpsetoption(s, "alpn", 1);
  if (pin && pin[0]) httpsetoption(s, "pin_sha256", pin);
  mstream out = mstreamcreate();
  int code = httprequest(s, "GET", url, NULL, out);
  printf("status=%d err=%d msg=%s\n", code, httperrorcode(s), httplasterror(s));
  mstreamfree(&out);
  httpclose(s);
  return 0;
}
```

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

## Socket networking

The VM also exposes thin wrappers over BSD sockets:

- `socketcreate(type[, family])` – create TCP (`0`) or UDP (`1`) sockets.
  Pass `6` (or an `AF_INET6` constant) for IPv6 sockets; the default is IPv4.
- `socketconnect(s, host, port)` – connect to a remote host/port.
- `socketbind(s, port)`, `socketlisten(s, backlog)`, `socketaccept(s)` – server helpers.
- `socketsend(s, data)` and `socketreceive(s, maxlen)` – send or receive strings or memory streams.
- `socketsetblocking(s, bool)` toggles blocking mode; `socketpoll(s, timeout_ms, flags)` polls for read (`1`) or write (`2`).
- `socketlasterror()` returns the last error code and `dnslookup(host)` resolves hostnames.

See `Examples/clike/base/SocketEchoDemo` for a complete echo server/client demo.
