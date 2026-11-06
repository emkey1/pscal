# Examples

Each front end keeps its example programs in its own component repository.
The entries here are links to them, so the whole set can be browsed from one
place:

| Directory | Front end | Source |
|---|---|---|
| `pascal/` | Pascal | `components/pascal/examples` |
| `clike/` | CLike | `components/clike/examples` |
| `rea/` | Rea | `components/rea/examples` |
| `aether/` | Aether | `components/aether/examples` |
| `exsh/` | exsh | `components/exsh/examples` |
| `tiny/` | tiny | here (it has no component of its own) |

Most front ends split their examples into `base/` (terminal programs) and
`sdl/` (graphics, needs a build with SDL). Run one with its front end, for
example `build/bin/pascal Examples/pascal/base/hello`.

The links need the submodules checked out (`git clone --recurse-submodules`).
Edit an example in its component, not through the link, when you mean to
commit the change: the change belongs to that component's repository.
