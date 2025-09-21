# Pscal v2.21

Date: 2025-10-02

## Highlights
- **3D SDL/OpenGL pipeline across front ends.** Pascal, CLike, and Rea now share a first-class 3D layer (`InitGraph3D`, fixed-function `GL*` helpers, swap control) with fresh demos ranging from cube spinners to an explorable terrain generator, making it far easier to ship hardware-accelerated showcases with one code path.【F:Docs/pscal_vm_builtins.md†L262-L314】【F:Docs/rea_language_reference.md†L114-L159】【F:Examples/clike/sdl_smoke†L1-L63】【F:Examples/rea/sdl_landscape†L310-L367】
- **HTTP stack handles hermetic `data:` URLs and IPv6.** Synchronous, file, and async requests parse `data:` payloads directly (including percent and base64 encoding), synthesize headers, and respect `socketCreate(..., family=6)` so demos gracefully fall back to IPv6-only hosts.【F:src/backend_ast/builtin_network_api.c†L371-L520】【F:src/backend_ast/builtin_network_api.c†L998-L1074】【F:src/backend_ast/builtin_network_api.c†L1404-L1445】【F:src/backend_ast/builtin_network_api.c†L2485-L2551】【F:Docs/clike_tutorial.md†L175-L183】
- **Caching and runtime fixes stabilize nested routines.** The bytecode cache now resolves front-end paths via `PATH`, embeds nested procedure metadata, and `CALL_USER_PROC` consults the hierarchical procedure table, eliminating vtable and constructor lookups that previously failed after warm starts.【F:src/core/cache.c†L21-L120】【F:src/core/cache.c†L574-L779】【F:src/core/cache.c†L792-L824】【F:src/core/cache.c†L860-L903】【F:src/vm/vm.c†L887-L907】【F:src/vm/vm.c†L4249-L4309】

## New
- **Audio control:** Added `StopAllSounds` so SDL builds can silence every channel and music stream without tearing down the mixer, matching the rest of the sound lifecycle helpers.【F:src/backend_ast/audio.c†L211-L299】【F:Docs/pscal_vm_builtins.md†L261-L267】
- **Performance suite:** New `Examples/Pascal/PerformanceBenchmark` stresses matrices, sorting, file I/O, sets, linked lists, and math kernels to profile VM and compiler optimizations with a single driver.【F:Examples/Pascal/PerformanceBenchmark†L1-L120】
- **3D showcase programs:** `Examples/Pascal/SDLSmoke`, `Examples/clike/sdl_smoke`, and `Examples/rea/sdl_demo` demonstrate the shared GL helpers with rotating cubes, vsync toggling, and window management boilerplate ready to copy.【F:Examples/Pascal/SDLSmoke†L1-L30】【F:Examples/clike/sdl_smoke†L1-L63】【F:Examples/rea/sdl_demo†L1-L84】

## Improvements
- **Bytecode caching:** Cache keys resolve interpreter binaries via `realpath`/`PATH`, and cache files serialize every nested procedure—including enclosing relationships and upvalues—so warm boots preserve VTable-backed objects and aliased routines.【F:src/core/cache.c†L21-L120】【F:src/core/cache.c†L574-L779】【F:src/core/cache.c†L792-L824】【F:src/core/cache.c†L860-L903】
- **Runtime calls:** `CALL_USER_PROC` now respects lexical visibility when hunting for a callee, which fixes nested Pascal constructors/methods invoked through `new` or procedure pointers after optimizations.【F:src/vm/vm.c†L887-L907】【F:src/vm/vm.c†L4249-L4309】
- **Memory streams:** Shared `MStream` instances carry reference counts and are retained when copied onto the VM stack, preventing double-frees and truncated buffers when builtins or async jobs reuse handles.【F:src/core/utils.c†L206-L237】【F:src/vm/vm.c†L822-L835】
- **Rea expressiveness:** The lexer/parser understand `nil` literals, XOR (`xor`/`^`) infix operators, and short-circuit lowering uses the VM’s `TO_BOOL`, keeping boolean flows canonical across OO demos.【F:src/rea/lexer.c†L94-L108】【F:src/rea/parser.c†L730-L736】【F:src/rea/parser.c†L1142-L1168】【F:src/rea/README.md†L9-L16】
- **Pascal globals:** The compiler defers global variable initializers until vtables exist, queues `new` expressions during the first pass, and replays them once class metadata is emitted so OO globals come up fully wired.【F:src/compiler/compiler.c†L162-L441】【F:src/compiler/compiler.c†L628-L669】【F:src/compiler/compiler.c†L3016-L3134】【F:src/compiler/compiler.c†L2448-L2508】
- **Faster field/index reads:** New `LOAD_FIELD_VALUE[*]` and `LOAD_ELEMENT_VALUE_CONST` opcodes let front ends pull fields and folded array elements without auxiliary address temporaries, shrinking common accessor patterns.【F:src/compiler/compiler.c†L5167-L5208】【F:Docs/pscal_vm_overview.md†L209-L224】
- **Networking polish:** `httpRequest`/`httpRequestToFile`/async workers synthesize deterministic headers for `data:` responses, stream decoded payloads into memory or files, and propagate IPv6-aware socket semantics through bind/connect helpers.【F:src/backend_ast/builtin_network_api.c†L371-L520】【F:src/backend_ast/builtin_network_api.c†L998-L1074】【F:src/backend_ast/builtin_network_api.c†L1404-L1445】【F:src/backend_ast/builtin_network_api.c†L2485-L2551】【F:src/backend_ast/builtin_network_api.c†L1784-L2095】

## Fixed
- **Zero-argument constructors:** `new` now always pushes the receiver before invoking constructors, even when no explicit arguments are present, so default constructors run for globals and locals alike.【F:src/compiler/compiler.c†L2448-L2508】
- **Global initializer ordering:** Deferred emission guarantees that object globals receive their vtables and constant data before user code observes them, addressing intermittent vtable lookup failures after peephole passes.【F:src/compiler/compiler.c†L162-L441】【F:src/compiler/compiler.c†L628-L669】【F:src/compiler/compiler.c†L3016-L3134】

## Documentation
- Expanded references cover the 3D SDL entry points, field/value load opcodes, and socket family parameter so tutorials match the new runtime surface area.【F:Docs/pscal_vm_builtins.md†L262-L314】【F:Docs/pscal_vm_overview.md†L209-L224】【F:Docs/clike_tutorial.md†L175-L183】

## Testing
- Regression suites picked up coverage for `data:` URL fetches and Rea short-circuit lowering, ensuring new opcodes and truthiness behavior stay stable.【F:Tests/clike/HttpFetchDataURL.cl†L1-L18】【F:Tests/rea/short_circuit.disasm†L1-L86】

## Build & Install
- Configure and build:
  - `cmake -S . -B build [-DSDL=ON] [-DRELEASE_BUILD=ON]`
  - `cmake --build build -j`
- Install:
  - `cmake --install build`
- Environment toggles:
  - `RUN_SDL=1` to enable SDL demos/tests, `RUN_NET_TESTS=1` for network suites.【F:README.md†L24-L88】【F:README.md†L96-L145】

## Known Notes
- The Rea front end remains experimental; expect ongoing rapid changes while its OO surface settles.【F:src/rea/README.md†L3-L16】
