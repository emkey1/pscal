# Aether showcase

This directory contains a more substantial Aether example than the small
one-file samples in `Examples/aether/base`.

Run it from the repository root with:

```sh
./build/bin/aether Examples/aether/showcase/agent_report
```

The showcase exercises the current Aether surface in combination:

- relative `use` imports across sibling Aether modules
- imported `const` values used through inferred `let` bindings
- exported helper functions from another Aether module
- `@pure`, `@pre`, and `@post`
- `type` blocks with methods and `self`
- inferred local bindings
- compact `loop` control flow
- `ToonDoc` / `ToonNode` parsing from a file payload
- typed TOON traversal and defaulted reads
- explicit `fx` output boundaries

If yyjson/TOON support is unavailable in the current build, the program exits
cleanly after reporting that capability gap.
