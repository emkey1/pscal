# Pscal v2.3.1

Date: 2025-10-05

## Highlights
- OpenAI extended built-ins now ship a first-party chat completion bridge for Pascal, C-like, and Rea with helper libraries and demos.
- Rea CLI grows a `--no-run` mode, parser/class fixes, and a brand-new programmer's guide to speed up onboarding.
- The OpenAI chat demo graduates into a generic OpenAI-compatible client with LM Studio presets, resilient JSON/UTF-8 handling, and transparent diagnostics.
- Runtime resilience improves via deeper Pascal unit fallbacks, portable builtin registry allocation, and restored `ReadKey` behaviour.

## New
- Extended built-ins
  - Added the optional `openai` category with `OpenAIChatCompletions(model, messagesJson, [optionsJson, apiKey, baseUrl])`, delivering direct `/chat/completions` access; toggle it with `-DENABLE_EXT_BUILTIN_OPENAI=ON/OFF`.
  - Shipped helper libraries under `lib/{pascal,clike,rea}/openai` so every front end can compose requests and extract assistant replies without hand-written JSON plumbing.
  - Documented the new category in `Docs/extended_builtins.md`, including env-var fallbacks to `OPENAI_API_KEY` when no key is supplied at call time.
- Examples & demos
  - Added `Examples/pascal/base/OpenAINewApiDemo`, demonstrating prompt assembly, option overrides, and environment-driven authentication for the new builtin.
  - Rebuilt the Rea chat sample into a reusable CLI that targets public OpenAI endpoints, local proxies, or LM Studio via system prompts, base/endpoint overrides, raw options JSON, and a `--lmstudio` shortcut.
- Documentation
  - Published `Docs/rea_programmers_guide.md` with tooling walkthroughs, compiler flags, and bytecode workflows for the Rea front end.
  - Expanded the Rea CLI documentation to cover new flags, disassembly guidance, and extended builtin discovery helpers.

## Improvements
- Rea front end & tooling
  - Added `--no-run` so `rea` can compile programs without launching the VM, and taught `Tests/run_rea_tests.sh` to exercise the flag alongside version, strict, and disassembly checks.
  - The regression harness now canonicalises repository paths, validates `--dump-ext-builtins` output, and shifts fixture mtimes safely for caching scenarios.
  - Scope verification suites picked up class-constructor coverage to guard object-oriented regressions.
- OpenAI chat tooling
  - Request builders precompute buffer sizes, use dynamic strings for defaults, and emit full payload/authorization diagnostics to speed up API troubleshooting.
  - LM Studio presets infer base URLs/endpoints, sanitise model identifiers, trim whitespace, and copy CLI arguments safely; a fallback selects the first advertised model when none is provided.
  - Secrets logged from demos are masked entirely, and HTTP 400 responses now replay payloads, URLs, and contextual hints without exposing credentials.
- Runtime & libraries
  - Pascal unit discovery probes additional relative directories so redistributed builds still locate `lib/pascal` without manual `--unit-path` tweaks.
  - The extended builtin registry replaced non-portable `strndup` usage with an explicit allocator, improving compatibility with stricter libcs.
  - Sample Fibonacci programs now exit with status `1` when rejecting oversized inputs to better signal failure to the shell.

## Fixed
- Rea language & CLI
  - Class constructors execute exactly once with the correct scope resolution, restoring behaviour relied upon by the new regression tests.
  - The parser now allows type keywords as identifiers in legal positions, eliminating spurious "unexpected keyword" errors.
  - `ReadKey` handling was restored after a bad merge so SDL keyboard input matches the 2.3.0 baseline.
- OpenAI integration
  - Chat payload builders allocate correctly for empty message lists, declare helpers before use, and honour UTF-8 multibyte sequences so streamed replies render intact.
  - LM Studio flows trim preset whitespace, sanitise model slugs, and honour presets without explicit models while copying CLI arguments reliably to the request payload.
  - Secrets shorter than four characters are now fully masked, and diagnostics summarise payloads and endpoints when servers reject a request.

## Build & Install
- Configure with `cmake -S . -B build [-DENABLE_EXT_BUILTIN_OPENAI=ON/OFF]` alongside the existing SDL and extended builtin toggles, then `cmake --build build`.

## Testing
- `Tests/run_rea_tests.sh` now enforces canonical casing, verifies extended builtin dumps, and exercises the new `--no-run` path; class scope regression fixtures backstop the constructor repairs.

## Compatibility
- `OpenAIChatCompletions` defaults to `https://api.openai.com/v1` and falls back to `OPENAI_API_KEY` when the API key parameter is empty; the Rea demo also honours `LLM_API_KEY` for local proxies.
- The LM Studio preset expects a server on `http://127.0.0.1:1234`; adjust the base URL or disable the preset when targeting other hosts.
- Disable the `openai` category at configure time when distributing to offline targets or toolchains without libcurl.

## Known Notes
- The OpenAI builtin returns raw JSON. Use the supplied helper libraries or the demos as references when integrating with custom front ends.
- Network access and valid credentials remain mandatory for the `openai` category; demo scripts surface 400-level hints but cannot recover from upstream configuration errors automatically.
