# Pscal Documentation Index

Start here to explore the available guides and references in this directory.

## Project overview
- [project_overview.md](project_overview.md): high-level architecture, features, and build instructions.
- [Architecture](Architecture): Mermaid diagram of system components (rendered as [Architecture.png](Architecture.png)).

## Pascal front end
The Pascal language docs now live in the [pascal](https://github.com/emkey1/pascal) repo under [`components/pascal/docs/`](../components/pascal/docs).
- [go_style_closure_interface_demo.md](go_style_closure_interface_demo.md): walkthrough of PSCAL's composition-first record/interface model and how closures fit into it.

## C-like front end
The CLike language docs now live in the [clike](https://github.com/emkey1/clike) repo under [`components/clike/docs/`](../components/clike/docs).

## exsh front end
The exsh docs now live in the [exsh](https://github.com/emkey1/exsh) repo under [`components/exsh/docs/`](../components/exsh/docs).
- [exsh_debug_log.md](exsh_debug_log.md): enabling and reading the structured debug log.

## Rea front end
The Rea language docs now live in the [rea](https://github.com/emkey1/rea) repo under [`components/rea/docs/`](../components/rea/docs).

## Aether front end
The Aether language docs now live in the [aether](https://github.com/emkey1/aether) repo under [`components/aether/docs/`](../components/aether/docs); training and benchmark notes are in [aether-infrastructure](https://github.com/emkey1/aether-infrastructure).

## Tiny compiler
- [tools/tiny](../tools/tiny): educational Python-based compiler front end.
- [clike_tiny_compiler.md](../components/clike/docs/clike_tiny_compiler.md): Tiny compiler implemented in CLike (`bin/tiny.clike`) and wrapper usage (`bin/tiny`).
- [tiny_language_expansion_tutorial.md](tiny_language_expansion_tutorial.md): practical guide for adding new Tiny language features to `bin/tiny.clike`.

## Virtual machine
The VM reference (overview, builtins, assembler) now lives in the [pscal-core](https://github.com/emkey1/pscal-core) repo under [`components/pscal-core/docs/`](../components/pscal-core/docs).
- [extended_builtins.md](extended_builtins.md): how to add custom built-in routines.
- [standalone_vm_frontends.md](standalone_vm_frontends.md): writing external frontends that emit Pscal bytecode.

## Networking and security
- [http_security.md](http_security.md): TLS, pinning, and proxy configuration for the HTTP APIs.
- [network_troubleshooting.md](network_troubleshooting.md): common issues when using the network APIs.
- [simple_web_server.md](simple_web_server.md): minimal CLike HTTP server for demos and local testing.

## Object model
- [object_layout.md](../components/pscal-core/docs/object_layout.md): runtime memory layout of objects.
