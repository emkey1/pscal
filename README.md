# PSCAL

**Practical Scripting Computation And Logic engine** — a bytecode VM with a
shared backend and multiple language front ends, written in C.

PSCAL is built around one idea: invest in a single stack-based virtual
machine, bytecode compiler, and builtin library, then keep language front ends
thin. Every front end gets the full backend for free — HTTP with serious TLS
controls, SQLite, yyjson, threads, optional SDL2/SDL3 graphics and audio, and
an extensible builtin mechanism. The AST-JSON → bytecode tool
(`pscaljson2bc`) makes the VM a public compilation target, so new front ends
can be prototyped in any language.

## Front ends

- **Aether** *(new, experimental)*: a compact language designed from the start
  to be written correctly by LLMs as well as humans. Explicit effect
  boundaries, lightweight contracts, a deliberately closed builtin surface,
  and first-class structured-data (TOON/JSON) helpers. See below.
- **Rea**: an object-oriented language.
- **Pascal**: a significant subset of classic Pascal.
- **CLike**: a C-like language with native strings and other enhancements.
- **exsh**: a shell front end that compiles orchestration scripts to PSCAL
  bytecode, with threads and access to the full builtin catalog.
- **tiny**: a minimal educational front end written in Python
  (`tools/tiny`), demonstrating how to target the VM externally.

PSCAL started as a Pascal interpreter and was written for the most part with
the help of various AIs — most notably Google's Gemini 2.5 Pro and, more
recently, OpenAI's GPT-5 with Codex. It has since evolved into the
multi-front-end VM described here.

## Aether: an LLM-first front end

Aether is the newest front end (it is less than a week old — expect rough
edges and rapid change). Its design goal is unusual: most languages optimize
for human ergonomics and treat machine generation as a side effect. Aether
inverts that. The language is specified so that a model given only its
reference document produces correct programs on the first try:

- **Visible effects**: all output, task launches, and AI calls live inside
  explicit `fx { ... }` blocks; pure logic stays outside.
- **Lightweight contracts**: `@pure`, `@pre`, `@post`, and `@cost`
  annotations with runtime checking.
- **Closed builtin surface**: the documented helpers are the complete
  callable surface, which removes the most common LLM failure mode —
  invented functions.
- **Structured data first**: TOON helpers (backed by yyjson) for parsing and
  traversing JSON-shaped payloads with opaque, type-safe handles.
- **Canonical forms**: one preferred spelling for each construct, with
  accepted compatibility forms documented separately, plus repair rules that
  map diagnostics back to fixes.

Aether lowers onto the shared backend through the existing toolchain;
diagnostics map back to original Aether source lines, with structured output
available via `--diagnostics-json` / `--diagnostics-toon` and compatibility
warnings via `--verbose-compat`.

A quick taste:

```aether
@pure
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    ret "review";
}

fn main() -> Void {
    let status: Text = classify(95);
    fx {
        println("status = ", status);
    }
    ret;
}
```

Documentation and examples:

- Full reference: `Docs/` [VERIFY: exact filename — the two Aether docs
  cross-reference each other as `aether_for_humans_and_llms.md` and
  `aether_for_llms_and_others.md`; settle on one name and link it here]
- Small-context LLM guide: `Docs/aether_for_llms_with_small_contexts.md`
- Examples: `Examples/aether/base/`, `Examples/aether/showcase/`
  (`agent_report`, `release_board`)

Example usage:

```
[VERIFY: invocation — dedicated binary (e.g. build/bin/aether
Examples/aether/showcase/agent_report) or via another front end's binary?
Show the exact command a new user should run first.]
```

Early internal testing suggests LLMs given only the Aether reference produce
correct programs at a higher rate than they produce correct Python for the
same tasks. [VERIFY: link the eval harness / benchmark scripts when
published; until then consider softening or omitting this claim in the
public README]

## Community

- Discord server: <https://discord.gg/jZV6UHUyBS>
- iOS/iPadOS TestFlight info: see the Discord `#testflight` channel
  [VERIFY: keep or restore the original deep link]

## Requirements

- C compiler with C11 support
- [CMake](https://cmake.org/) 3.18 or newer
- [Git](https://git-scm.com/) (required for submodules)
- [libcurl](https://curl.se/libcurl/) when building with
  `-DPSCAL_USE_BUNDLED_CURL=OFF`
- **Optional**: SDL2 or SDL3 plus the matching `SDL*_image`, `SDL*_mixer`
  and `SDL*_ttf` libraries when building with `-DSDL=ON`

On Debian/Ubuntu:

```
sudo apt-get update
sudo apt-get install build-essential cmake git libcurl4-openssl-dev \
    libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev libsdl2-ttf-dev
```

## Clone (with submodules)

```
git clone --recurse-submodules https://github.com/emkey1/pscal.git
cd pscal
```

If you already cloned without submodules:

```
git submodule update --init --recursive
```

After each pull:

```
git pull --recurse-submodules
git submodule update --init --recursive
```

## Building

```
cmake -S . -B build
cmake --build build -j
```

Common configure options:

- `-DSDL=ON`: enable SDL-dependent graphics/audio builtins and examples.
- `-DPSCAL_USE_SDL3=ON`: prefer SDL3 where supported.
- `-DRELEASE_BUILD=ON`: append `_REL` to version naming while keeping
  optional extended builtins enabled.
- `-DBUILD_DASCAL=ON`: build the debug-oriented `dascal` binary (very
  verbose debugging; not built by default).

Binaries are written to `build/bin` (e.g. `pascal`).

If you use submodules, verify pinned commits are fetchable before pushing:

```
Tools/check_submodule_refs.sh
Tools/check_submodule_refs.sh --protected-refs   # both protected branches
```

To build without SDL explicitly:

```
cmake -S . -B build -DSDL=OFF
cmake --build build -j
```

### iOS/iPadOS

PSCAL ships with an iOS/iPadOS host app built in SwiftUI
(`ios/PSCAL.xcodeproj`). The app embeds the toolchain, stages examples and
runtime assets into the app sandbox, and is available for external testing
through TestFlight. Static-library presets:

```
cmake --preset ios-simulator-debug   && cmake --build --preset ios-simulator-debug
cmake --preset ios-device-debug      && cmake --build --preset ios-device-debug
cmake --preset ios-simulator-release && cmake --build --preset ios-simulator-release
cmake --preset ios-device-release    && cmake --build --preset ios-device-release
```

See `ios/README.md` and `Docs/ios_build.md` for a full walkthrough.

## Repository layout (git)

PSCAL uses a mix of submodules and vendored source trees:

- Submodules (pinned in `.gitmodules`): `src/smallclue`, `third-party/SDL`,
  `third-party/micro`, `third-party/dvtm`, `third-party/libgit2`,
  `third-party/openrsync`.
- Vendored in-tree sources: notably `third-party/nextvi` and
  `third-party/openssh-10.2p1`.

SmallCLUE note: `src/smallclue/third-party` is bootstrap-generated and
intentionally not tracked as submodules; it is populated by the SmallCLUE
setup scripts via `src/smallclue/fetch_dependencies.sh`. For applet coverage
(`git`, `ssh/scp/sftp`, `rsync`, etc.) see `src/smallclue/README.md`.

## Tests

After building:

```
./Tests/run_all_tests
```

The harness auto-selects a writable `TMPDIR` so it can be launched from any
working directory.

- **Headless defaults**: with SDL enabled at build time, test runners default
  to dummy SDL drivers in headless/CI environments; SDL-dependent tests are
  skipped.
- **Force SDL tests** with a real driver:

  ```
  # macOS (logged-in GUI session)
  RUN_SDL=1 SDL_VIDEODRIVER=cocoa SDL_AUDIODRIVER=coreaudio ./run_all_tests
  # Linux (X11)
  RUN_SDL=1 SDL_VIDEODRIVER=x11 ./run_all_tests
  ```

- **Network tests** are guarded for CI determinism; set `RUN_NET_TESTS=1` to
  enable them (e.g. `Examples/pascal/base/HttpHeadersNetDemo`; the CLike
  runner skips tests with a `.net` sentinel file unless it is set).

Note: on macOS you may see benign LaunchServices/XPC warnings on stderr when
running SDL tests in some environments.

## Front end details

### Rea

Rea is PSCAL's object-oriented front end. The install step publishes the Rea
import library to `${CMAKE_INSTALL_PREFIX}/lib/rea`.
[VERIFY: the original README never gave Rea its own usage section — worth a
short one here with a binary invocation and an Examples path, since Aether
lowers through it]

### CLike

`build/bin/clike` implements a compact C-like bytecode compiler. The grammar
covers variable and function declarations, conditionals, loops and
expressions. VM builtins can be invoked simply by calling a function name that
lacks a user definition.

```
build/bin/clike program.cl
```

Sample programs live in `Examples/clike/base`; see `Docs/clike_tutorial.md`
for a step-by-step guide. An interactive session is available via
`build/bin/clike-repl` (wraps a line in `int main() { ... }` and executes it;
see `Docs/clike_repl_tutorial.md`).

Options and semantics:

- `--dump-bytecode`: compile and disassemble bytecode, then execute.
- `--dump-bytecode-only`: disassemble, then exit.
- `pscald <bytecode_file>`: standalone disassembler (output matches
  `--dump-bytecode-only`).
- Logical `&&` / `||` short-circuit; `<<` / `>>` supported with standard
  precedence; `~x` is bitwise NOT on integers, logical NOT otherwise.
- With `-DSDL=ON` the preprocessor defines `SDL_ENABLED` for `#ifdef` guards.

Environment variables:

- `CLIKE_LIB_DIR`: search directory for CLike `import "..."` modules.
- `PASCAL_LIB_DIR`: root directory for Pascal units (`.pl` files).
- `SDL_VIDEODRIVER` / `SDL_AUDIODRIVER`: `dummy` by default in headless runs.
- `RUN_NET_TESTS=1`: enables network-dependent tests and demos.

A minimal HTTP server written in CLike lives at
`Examples/clike/base/simple_web_server` (worker pool, metrics, `index.html`
auto-detection; docs in `Docs/simple_web_server.md`; a basic `htdocs` lives
under `lib/misc/simple_web_server/htdocs`):

```
build/bin/clike Examples/clike/base/simple_web_server [port] [/path/to/htdocs] [threads] [queue]
```

### Pascal

Implements a significant subset of classic Pascal. A demo exercising
procedure/function pointers and the `CreateThread(@Proc, arg)` /
`WaitForThread(t)` APIs lives at `Examples/pascal/base/ThreadsProcPtrDemo`:

```
cmake --build build --target run_threads_procptr_demo
# or
make -C Examples threads-procptr-demo
```

### exsh

`build/bin/exsh` compiles shell-style orchestration scripts to PSCAL
bytecode. Pipelines, background jobs, and conditionals map to dedicated VM
builtins (`backend_ast/shell.c`), and the full PSCAL builtin catalog is
available via the `builtin` command. Prefix arguments with `int:`,
`float:`/`double:`/`real:`, `bool:`, `str:` or `nil` to coerce shell tokens.

```
build/bin/exsh Examples/exsh/pipeline
build/bin/exsh --dump-bytecode Examples/exsh/functions
build/bin/exsh Examples/exsh/builtins
```

Bytecode is cached in `~/.pscal/bc_cache` (`--no-cache` to force
recompilation). The runtime exports `EXSH_LAST_STATUS` after every builtin
invocation. `export`/`unset` mutate the process environment for subsequent
commands. Direct parameter interpolation (`$NAME`, `${NAME}`) is parsed but
not yet expanded. **Known limitation**: control-flow helpers (`if`, loop
syntax) are currently placeholders that execute both branches — gate behavior
with `EXSH_LAST_STATUS` until the VM gains jump support for exsh.

Threading demos: `Examples/exsh/threading_demo` and
`Examples/exsh/parallel-check` show `ThreadSpawnBuiltin` / `WaitForThread` /
`ThreadGetResult` / `ThreadSetName`.

For iOS/vproc parity testing on macOS and the release sanity sweep, see
`Tests/run_exsh_ios_host_tests.sh`, `Tests/run_ios_release_sanity.sh`, and
`Docs/exsh_overview.md`.

### tiny

A minimal compiler for a small educational language, written in Python
(`tools/tiny`). Integer variables and arithmetic only — an example of adding
a standalone front end that emits VM bytecode:

```
python tools/tiny program.tiny out.pbc
./build/bin/pscalvm out.pbc
./build/bin/pscald out.pbc
```

Example programs live in `Examples/tiny`.

## AST JSON → Bytecode (pscaljson2bc)

`pscaljson2bc` compiles an AST JSON stream (as produced by any front end's
`--dump-ast-json`) into VM bytecode — this is the supported path for building
external front ends.

- Input: a file path or `-`/stdin. Output: raw bytecode to stdout or
  `-o <file>`.
- `--dump-bytecode` disassembles to stderr before writing;
  `--dump-bytecode-only` disassembles and exits.

```
build/bin/pascal --dump-ast-json Tests/Pascal/BoolTest | build/bin/pscaljson2bc -o out.bc
build/bin/pscalvm out.bc
build/bin/pscald out.bc
```

Installed alongside `pascal` and `pscalvm` by `cmake --install`. Shell
completions live in `tools/completions` (bash and zsh; CMake can install them
via `PSCAL_INSTALL_COMPLETIONS`). Full guide: `Docs/pscaljson2bc.md`.

## HTTP networking

Built-in HTTP helpers are available to all front ends. Highlights:

- Sessions: `HttpSession`, `HttpClose`. Headers: `HttpSetHeader`,
  `HttpClearHeaders`, `HttpGetLastHeaders`, `HttpGetHeader`.
- Requests: `HttpRequest(s, method, url, body, outMStream)` → status code;
  `HttpRequestToFile(...)` for file output. Async jobs snapshot session
  options at submission.
- Options via `HttpSetOption`: `timeout_ms`, `follow_redirects`,
  `user_agent`, `accept_encoding`, `out_file`, `http2`, `basic_auth`.
- TLS: `ca_path`, `client_cert`/`client_key`, `verify_peer`/`verify_host`,
  `tls_min`/`tls_max` (10/11/12/13), `alpn`, `ciphers`, and SPKI pinning via
  `pin_sha256` (`sha256//BASE64` or a file path).
- Proxies: `proxy`, `proxy_userpwd`, `proxy_type` (`http`, `https`,
  `socks5`, `socks4`).
- DNS overrides: `resolve_add` (`host:port:address`), `resolve_clear`.
- Errors: `HttpErrorCode` (0 none; 1 generic; 2 I/O; 3 timeout; 4 SSL;
  5 resolve; 6 connect) and `HttpLastError`.
- `file://` URLs are handled directly by the runtime with synthesized
  headers, enabling hermetic tests.

See `Docs/http_security.md` for pinning, TLS knobs, proxies, and DNS
overrides with step-by-step commands. `tools/pin-from-host.sh` computes a
libcurl-compatible SPKI pin from a live host or PEM file.

## Runtime library

The front ends ship with fonts, configuration templates, documentation, and
standard libraries:

```
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/opt/pscal
cmake --build build
sudo cmake --install build
```

Executables install to `${CMAKE_INSTALL_PREFIX}/bin`; runtime assets to
`${PSCAL_INSTALL_ROOT}` (default `${CMAKE_INSTALL_PREFIX}/pscal`, overridable
with `-DPSCAL_INSTALL_ROOT=...`). When `exsh` is the active login shell, the
installer backs up the running binary to `exsh.previous` before upgrading.

## Extending built-ins

Drop C source files into `src/ext_builtins`; each implements a
`registerExtendedBuiltins` function. See `Docs/extended_builtins.md` and the
example exposing the host PID in `src/ext_builtins/getpid.c`.

## License

As the PSCAL code base was primarily generated by AI, it is distributed under
the [MIT License](LICENSE). Releases prior to 2.22 were distributed under the
Unlicense.
