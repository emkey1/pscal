# PSCAL and Its Educational Applications

If **PSCAL** has any real-world application, it is most likely in the area of education. The way the PSCAL suite works is very much analogous to the way the GNU compilers work. Both systems follow a multi-stage compilation process.

In the **PSCAL suite**, the front ends (like Pascal) translate source code into an intermediate representation, specifically an **Abstract Syntax Tree (AST)**. The AST is a structured, platform-independent form of the program. This is similar to how a GNU compiler first translates source code into its own intermediate format.

The next step in the PSCAL suite is the **bytecode compiler**, which takes the AST and generates **PSCAL bytecode**. This bytecode is not native machine code but is designed to run on the **PSCAL virtual machine (VM)**. This design allows the compiled program to be executed on any system that has the PSCAL VM, much like how a program compiled by a GNU compiler can run on any system with the same CPU architecture.

---

## Educational Opportunities

### 1. Transparency of the Compilation Process

Unlike a traditional compiler that might hide the intermediate steps, the PSCAL suite’s transparent pipeline allows students to see a direct and clean mapping of a high-level language like Rea onto the PSCAL VM’s stack-based runtime.

This is a powerful learning tool for understanding how code is transformed from a human-readable format into machine-executable instructions. The `--dump-ast-json` and `--dump-bytecode` command-line flags provide a concrete way to inspect these compilation artifacts, which is invaluable for hands-on learning.

### 2. Understanding Virtual Machines

By targeting a virtual machine, the PSCAL suite provides a clear illustration of how a VM abstracts away the underlying hardware. Students can learn how an intermediate bytecode language can be executed on different platforms—a foundational concept in areas like Java’s JVM or the .NET Common Language Runtime.

### 3. Compiler Design and Theory

The separation between the language front end (lexer, parser, semantic analysis) and the bytecode compiler/VM makes the PSCAL suite an excellent subject for studying compiler design.

Students can examine each component:

* **Lexical analysis**
* **Parsing**
* **Semantic analysis** with a class-aware symbol table
* **Code generation**

The design goal of having compiled programs map cleanly onto PSCAL bytecode further reinforces the lessons in efficient code generation.

---

## Quick Start (showing the pipeline)

Here is a tiny **Pascal front end** program that maps cleanly to the VM. You can substitute your own file—these commands are what instructors/students can actually run to see the artifacts end to end.

- Save as `example.p`:
  ```pascal
  program example;
  var a: integer;
  begin
    a := 5;
    write('a = ', a, '\n');
    halt(0);
  end.
  ```
- Dump the AST: `build/bin/pascal --dump-ast-json example.p > ast.json`
- Dump the bytecode: `build/bin/pascal --dump-bytecode example.p > bytecode.txt`
- Run it: `build/bin/pascal example.p`

---

### AST Representation

Here is an **actual AST JSON excerpt** from the Pascal front end for that program (truncated for brevity):

```json
{
  "node_type": "PROGRAM",
  "children": [
    {
      "node_type": "FUNCTION_DECL",
      "token": {"value": "main"},
      "children": [
        {"node_type": "VAR_DECL", "token": {"value": "a"}, "var_type_annotated": "INTEGER"},
        {"node_type": "EXPR_STMT", "token": {"value": "write"}, "...": "..."},
        {"node_type": "EXPR_STMT", "token": {"value": "halt"}, "...": "..."}
      ]
    }
  ]
}
```

---

### Bytecode Compilation

Here is the **bytecode disassembly** produced from the AST:

```
Offset Line Opcode           Operand  Value / Target (Args)
------ ---- ---------------- -------- --------------------------
0000    0 CALL_USER_PROC       0 'main' @0005 (0 args)
0004    | HALT

--- Routine main (at 0005) ---
0005    2 CONSTANT            1 '5'
0007    | SET_LOCAL           0 (slot)
0009    3 CONSTANT            2 '0'
0011    | CONSTANT            3 'a = '
0013    | GET_LOCAL           0 (slot)
0015    | CONSTANT            4 '\n'
0017    | CALL_BUILTIN_PROC   176 'write' (4 args)
0023    | CONSTANT            2 '0'
0025    4 POP
0026    | CONSTANT            2 '0'
0028    | CALL_BUILTIN_PROC   124 'halt' (1 args)
0034    5 POP
0035    1 RETURN
== End Disassembly: /tmp/foo1 ==

Constants (7):\n  0000: STR   "main"
  0001: INT   5
  0002: INT   0
  0003: STR   "a = "
  0004: STR   "\n"
  0005: STR   "write"
  0006: STR   "halt"

```

---

### Execution on the VM

Finally, the compiled program runs on the PSCAL virtual machine:

```
--- Executing Program with VM ---
a = 5
```

This transparent workflow lets learners trace a program from source code through compilation to execution and inspect each intermediate form. Instructors can dissect the AST to explain parsing decisions, show how each bytecode instruction manipulates the VM stack, and connect those operations to the printed result. By stepping through each representation, students build an intuition for how high-level constructs translate into runtime behaviour.

## Extensibility

PSCAL’s code base is intentionally small and written in C, making it easy to extend. New language front ends or VM built-ins can be added with minimal scaffolding, providing a practical playground for exploring language and runtime design. Because the modules are loosely coupled, a single change—such as adding a new opcode or a parser rule—can be studied in isolation before being reintroduced into the whole system. This modularity encourages experimentation and helps learners see how individual compiler pieces cooperate to form a complete toolchain.

The project’s simplicity also invites collaboration. A class might divide into teams, each implementing a feature, and then merge their contributions to extend the platform. Since every front end targets the same VM, work on one language benefits the others, reinforcing how shared abstractions make language design more efficient.

### Why PSCAL (vs. existing teaching VMs?)
- Transparent AST/bytecode dumps via `--dump-ast-json`/`--dump-bytecode` on every front end, so students can see the whole pipeline without extra tooling.
- Small, hackable C codebase with shared VM across front ends (Pascal, C-like, Rea, exsh), making cross-language experiments cheap.
- Bytecode and VM are stable enough for course material, but not a general-purpose JVM/.NET replacement; UI/SDL and iOS constraints still apply.

### Notes and limits
- The examples above use the Pascal front end; exsh, C-like, and Rea can emit the same artifacts with the same flags.
- Some tooling is intentionally minimal (no full IDE); expect to drive dumps and bytecode inspection from the CLI.
message.md
8 KB

---
