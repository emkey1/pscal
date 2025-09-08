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

## Rea front end
- [rea_overview.md](rea_overview.md): architecture and roadmap of the experimental Rea compiler.
- [rea_language_reference.md](rea_language_reference.md): full specification of the Rea language.

## Tiny compiler
- [tools/tiny](../tools/tiny): educational Python-based compiler front end.

## Virtual machine
- [pscal_vm_overview.md](pscal_vm_overview.md): stack-based VM architecture and opcode reference.
- [pscal_vm_builtins.md](pscal_vm_builtins.md): catalog of built-in functions provided by the VM.
- [extended_builtins.md](extended_builtins.md): how to add custom built-in routines.
- [standalone_vm_frontends.md](standalone_vm_frontends.md): writing external frontends that emit Pscal bytecode.

## Networking and security
- [http_security.md](http_security.md): TLS, pinning, and proxy configuration for the HTTP APIs.
- [network_troubleshooting.md](network_troubleshooting.md): common issues when using the network APIs.
- [simple_web_server.md](simple_web_server.md): minimal CLike HTTP server for demos and local testing.

## Object model
- [object_layout.md](object_layout.md): runtime memory layout of objects.

