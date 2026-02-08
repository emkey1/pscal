# Pscal Documentation Index

Start here to explore the available guides and references in this directory.

## Project overview
- [project_overview.md](project_overview.md): high-level architecture, features, and build instructions.
- [Architecture](Architecture): Mermaid diagram of system components (rendered as [Architecture.png](Architecture.png)).

## Pascal front end
- [pascal_overview.md](pascal_overview.md): architecture and language features of the Pascal-style compiler.
- [pascal_language_reference.md](pascal_language_reference.md): full specification of the Pascal-like language.

## C-like front end
- [clike_overview.md](clike_overview.md): semantics and capabilities of the compact C-style language.
- [clike_language_reference.md](clike_language_reference.md): detailed specification of the C-like language.
- [clike_tutorial.md](clike_tutorial.md): build and run the C-like compiler.
- [clike_repl_tutorial.md](clike_repl_tutorial.md): interact with the language through the REPL.

## exsh front end
- [exsh_overview.md](exsh_overview.md): running shell scripts on the PSCAL VM, caching and builtin integration for exsh.
- [exsh_debug_log.md](exsh_debug_log.md): enabling and reading the structured debug log.

## Rea front end
- [rea_overview.md](rea_overview.md): architecture and roadmap of the experimental Rea compiler.
- [rea_language_reference.md](rea_language_reference.md): full specification of the Rea language.
- [rea_programmers_guide.md](rea_programmers_guide.md): practical workflows for building, testing, and extending Rea.
- [rea_tutorial.md](rea_tutorial.md): build and run the Rea compiler with an SDL sample.

## Tiny compiler
- [tools/tiny](../tools/tiny): educational Python-based compiler front end.
- [clike_tiny_compiler.md](clike_tiny_compiler.md): Tiny compiler implemented in CLike (`bin/tiny.clike`) and wrapper usage (`bin/tiny`).
- [tiny_language_expansion_tutorial.md](tiny_language_expansion_tutorial.md): practical guide for adding new Tiny language features to `bin/tiny.clike`.

## Virtual machine
- [pscal_vm_overview.md](pscal_vm_overview.md): stack-based VM architecture and opcode reference.
- [pscal_vm_builtins.md](pscal_vm_builtins.md): catalog of built-in functions provided by the VM.
- [extended_builtins.md](extended_builtins.md): how to add custom built-in routines.
- [standalone_vm_frontends.md](standalone_vm_frontends.md): writing external frontends that emit Pscal bytecode.
- [pscalasm.md](pscalasm.md): assemble `.pbc` files from canonical `PSCALASM2` text (`pscald --emit-asm`), with legacy `--asm` block compatibility.

## Networking and security
- [http_security.md](http_security.md): TLS, pinning, and proxy configuration for the HTTP APIs.
- [network_troubleshooting.md](network_troubleshooting.md): common issues when using the network APIs.
- [simple_web_server.md](simple_web_server.md): minimal CLike HTTP server for demos and local testing.

## Object model
- [object_layout.md](object_layout.md): runtime memory layout of objects.
