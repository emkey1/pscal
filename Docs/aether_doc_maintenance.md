# Aether Doc Maintenance Notes

This file is for human maintainers. It should not be appended to the LLM-facing
reference documents.

## Small-context LLM doc extraction checklist

When updating `Docs/aether_for_llms_with_small_contexts.md`, preserve these
items from the full reference:

- Highest-Value Rules
- Never Generate These
- Canonical vs accepted forms for high-risk syntax
- smallest useful program
- safe generation algorithm
- `fx` rules
- print/formatting rules
- conservative inference policy
- import rules
- TOON handle rules
- TOON root-shape rule
- TOON `_or` fallback warning
- tuple limitations
- repair rules
- validation checklist

Do not let the small-context document become a general overview. Its job is to
make generated Aether valid.

## Where to look next

Practical examples:

- `Examples/aether/base/README.md`
- `Examples/aether/showcase/README.md`

Implementation notes:

- `src/aether/README.md`
- `src/aether/DESIGN.md`

Best example files to copy from:

- `Examples/aether/base/hello`
- `Examples/aether/base/contracts`
- `Examples/aether/base/contract_layouts`
- `Examples/aether/base/inferred_decls`
- `Examples/aether/base/function_inference`
- `Examples/aether/base/object_inference`
- `Examples/aether/base/self_mutation`
- `Examples/aether/base/toon_access`
- `Examples/aether/base/toon_defaults`
- `Examples/aether/showcase/agent_report`

## Bottom Line

If you want valid Aether today, keep generated code compact, explicit about
effects, modest about inference, typed around TOON handles and extracted values,
careful about imports, conservative with tuples, and close to the existing
examples. That is still the fastest path for both humans and LLMs to produce
working Aether code.
