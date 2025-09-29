#!/usr/bin/env python3
"""Generate manifest.json for the Rea scope conformance suite."""
from __future__ import annotations

import json
from pathlib import Path
import textwrap

ROOT = Path(__file__).resolve().parent
MANIFEST_PATH = ROOT / "manifest.json"

tests = []


def normalise(text: str) -> str:
    return textwrap.dedent(text).strip("\n")


def add(test):
    item = dict(test)
    item["code"] = normalise(test["code"])
    if "expected_stdout" in item:
        item["expected_stdout"] = normalise(item["expected_stdout"])
    if "expected_stderr_substring" in item:
        item["expected_stderr_substring"] = item["expected_stderr_substring"].strip()
    if "files" in item:
        fixed_files = []
        for extra in item["files"]:
            fixed = dict(extra)
            fixed["code"] = normalise(extra["code"])
            if "placeholders" in fixed:
                fixed["placeholders"] = extra["placeholders"]
            fixed_files.append(fixed)
        item["files"] = fixed_files
    tests.append(item)


# ---------------------------------------------------------------------------
# Category A: Block and lexical scope
# ---------------------------------------------------------------------------

add({
    "id": "block_shadow_preserves_outer",
    "name": "Shadowing preserves outer value",
    "category": "block_scope",
    "description": "Inner block shadows outer variable without mutating the outer binding.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int outer = 1;
            printf("outer=%d, ", outer);
            {
                int outer = 2;
                printf("inner=%d, ", outer);
            }
            printf("outer_after=%d", outer);
            return 0;
        }
    """,
    "expected_stdout": "outer=1, inner=2, outer_after=1",
})

add({
    "id": "block_conditional_inner_isolated",
    "name": "Conditional block keeps locals isolated",
    "category": "block_scope",
    "description": "Variables declared inside a conditional do not leak and outer bindings stay intact.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int flag = 1;
            int total = 0;
            if (flag > 0) {
                int flag = 5;
                total = total + flag;
            }
            writeln("total=", total);
            writeln("flag=", flag);
            return 0;
        }
    """,
    "expected_stdout": """
        total=5
        flag=1
    """,
})

add({
    "id": "block_loop_variable_redeclare",
    "name": "Loop variable confined to loop scope",
    "category": "block_scope",
    "description": "Loop index is confined to the loop body so a new binding may reuse the name afterward.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int sum = 0;
            for (int i = 0; i < 3; i = i + 1) {
                sum = sum + i;
            }
            int i = 42;
            writeln("loop_sum=", sum, ", after=", i);
            return 0;
        }
    """,
    "expected_stdout": "loop_sum=3, after=42",
})

add({
    "id": "block_inner_reference_error",
    "name": "Referencing inner binding after block",
    "category": "block_scope",
    "description": "Using a block-local variable after the block should be rejected.",
    "expect": "compile_error",
    "code": """
        int main() {
            {
                int hidden = 3;
            }
            return hidden;
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Block locals must not be reachable after the block closes.",
})

add({
    "id": "block_duplicate_declaration_error",
    "name": "Duplicate binding within same scope",
    "category": "block_scope",
    "description": "Redeclaring an identifier in the same block is illegal.",
    "expect": "compile_error",
    "code": """
        int main() {
            int value = 1;
            int value = 2;
            return value;
        }
    """,
    "expected_stderr_substring": "duplicate",
    "failure_reason": "The compiler must reject duplicate declarations in one scope.",
})

add({
    "id": "block_loop_variable_leak_error",
    "name": "Loop index not visible outside",
    "category": "block_scope",
    "description": "Loop indices cannot be referenced after the loop completes.",
    "expect": "compile_error",
    "code": """
        int main() {
            for (int i = 0; i < 2; i = i + 1) {
                writeln(i);
            }
            return i;
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "For-loop locals must not leak into the enclosing scope.",
})

add({
    "id": "block_random_shadowing_ok",
    "name": "Randomised shadowing respects scope",
    "category": "block_scope",
    "description": "Random identifiers and nested braces still preserve lexical scope.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int {{outer_name}} = 10;
{{shadow_block}}
            writeln("after=", {{outer_name}});
            return 0;
        }
    """,
    "expected_stdout": """
        shadow-ok
        after=10
    """,
    "placeholders": {
        "outer_name": {"type": "identifier", "min_length": 5, "ensure_unique": True},
        "shadow_block": {
            "type": "nested_block",
            "indent": "    ",
            "min_depth": 1,
            "max_depth": 3,
            "header": "if (true)",
            "placeholders": {
                "shadow_name": {"type": "identifier", "min_length": 5, "avoid_placeholders": ["outer_name"]},
                "delta_val": {"type": "int", "min": 1, "max": 3},
                "end_comment": {"type": "comment", "words": ["random", "block"]},
            },
            "body": [
                "int {{shadow_name}} = {{outer_name}} + {{delta_val}};",
                "if ({{shadow_name}} > 0) {",
                "    writeln(\"shadow-ok\");",
                "}"
            ],
            "trailing": "{{end_comment}}",
        },
    },
})

add({
    "id": "block_random_leak_error",
    "name": "Randomised leak is rejected",
    "category": "block_scope",
    "description": "Random identifiers still cannot escape their block scope.",
    "expect": "compile_error",
    "code": """
        int main() {
            if (true) {
                int {{temp_name}} = 5;
                writeln({{temp_name}});
            }
            return {{temp_name}};
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Identifiers introduced in conditional blocks must not be visible outside.",
    "placeholders": {
        "temp_name": {"type": "identifier", "min_length": 4},
    },
})

# ---------------------------------------------------------------------------
# Category B: Function and procedure scope
# ---------------------------------------------------------------------------

add({
    "id": "function_parameter_shadows_global",
    "name": "Parameter shadows outer binding",
    "category": "function_scope",
    "description": "Function parameters hide globals of the same name without mutating them.",
    "expect": "runtime_ok",
    "code": """
        int total = 7;
        int bump(int total) {
            return total + 1;
        }
        int main() {
            writeln("bump=", bump(3));
            writeln("global=", total);
            return 0;
        }
    """,
    "expected_stdout": """
        bump=4
        global=7
    """,
})

add({
    "id": "function_default_param_reads_outer",
    "name": "Default argument sees lexical scope",
    "category": "function_scope",
    "description": "Default parameter expressions evaluate in the enclosing lexical environment.",
    "expect": "runtime_ok",
    "code": """
        const int BASE = 5;
        int adjust(int value = BASE) {
            return value + 2;
        }
        int main() {
            writeln("default=", adjust());
            writeln("explicit=", adjust(3));
            return 0;
        }
    """,
    "expected_stdout": """
        default=7
        explicit=5
    """,
})

add({
    "id": "function_mutual_recursion_scope",
    "name": "Mutual recursion keeps locals private",
    "category": "function_scope",
    "description": "Nested recursion should not pollute outer scopes or rely on shared locals.",
    "expect": "runtime_ok",
    "code": """
        int oddSum(int n);
        int evenSum(int n) {
            if (n <= 0) {
                return 0;
            }
            return n + oddSum(n - 1);
        }
        int oddSum(int n) {
            if (n <= 0) {
                return 0;
            }
            return n + evenSum(n - 1);
        }
        int main() {
            writeln("sum=", evenSum(4));
            return 0;
        }
    """,
    "expected_stdout": "sum=10",
})

add({
    "id": "function_duplicate_parameter_error",
    "name": "Duplicate parameter names rejected",
    "category": "function_scope",
    "description": "A function may not define two parameters with the same identifier.",
    "expect": "compile_error",
    "code": """
        int clash(int value, int value) {
            return value;
        }
        int main() {
            return clash(1, 2);
        }
    """,
    "expected_stderr_substring": "duplicate",
    "failure_reason": "Parameter lists must have unique identifiers per function signature.",
})

add({
    "id": "function_parameter_leak_error",
    "name": "Parameter visibility limited to function",
    "category": "function_scope",
    "description": "Parameters cannot be referenced outside of their defining function.",
    "expect": "compile_error",
    "code": """
        int box(int payload) {
            return payload + 1;
        }
        int main() {
            box(41);
            return payload;
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Function parameters must not leak into caller scope.",
})

add({
    "id": "function_forward_reference_local_error",
    "name": "Locals are not globally visible",
    "category": "function_scope",
    "description": "Another function cannot access a local variable defined elsewhere.",
    "expect": "compile_error",
    "code": """
        int helper() {
            return scratch;
        }
        int main() {
            int scratch = 9;
            return helper();
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Locals belong to their function activation only.",
})

add({
    "id": "function_random_shadowing_ok",
    "name": "Randomised parameter shadow",
    "category": "function_scope",
    "description": "Seeded random identifiers still respect function scope and globals.",
    "expect": "runtime_ok",
    "code": """
        int {{global_name}} = 2;
        int {{func_name}}(int {{param_name}}) {
            int {{local_name}} = {{param_name}} + {{global_name}};
            writeln("inner-ok");
            return {{param_name}} - {{global_name}};
        }
        int main() {
            writeln("result=", {{func_name}}(5));
            writeln("global=", {{global_name}});
            return 0;
        }
    """,
    "expected_stdout": """
        inner-ok
        result=3
        global=2
    """,
    "placeholders": {
        "global_name": {"type": "identifier", "min_length": 5},
        "func_name": {"type": "identifier", "min_length": 5, "avoid_placeholders": ["global_name"]},
        "param_name": {"type": "identifier", "min_length": 4, "avoid_placeholders": ["global_name", "func_name"]},
        "local_name": {"type": "identifier", "min_length": 4, "avoid_placeholders": ["global_name", "func_name", "param_name"]},
    },
})

add({
    "id": "function_random_leak_error",
    "name": "Randomised parameter leak rejected",
    "category": "function_scope",
    "description": "Seeded random names still cannot leak parameters outside the function.",
    "expect": "compile_error",
    "code": """
        int {{builder_name}}(int {{param_name}}) {
            return {{param_name}};
        }
        int main() {
            {{builder_name}}(1);
            return {{param_name}};
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Function parameters are local to their defining function.",
    "placeholders": {
        "builder_name": {"type": "identifier", "min_length": 5},
        "param_name": {"type": "identifier", "min_length": 4},
    },
})

# ---------------------------------------------------------------------------
# Category C: Closures and captured variables
# ---------------------------------------------------------------------------

add({
    "id": "closure_reference_capture_runtime",
    "name": "Closure captures by reference",
    "category": "closure_scope",
    "description": "A nested function observes the updated outer variable on subsequent calls.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int base = 3;
            int add(int delta) {
                return base + delta;
            }
            writeln("first=", add(2));
            base = 10;
            writeln("second=", add(1));
            return 0;
        }
    """,
    "expected_stdout": """
        first=5
        second=11
    """,
})

add({
    "id": "closure_incrementer_stateful",
    "name": "Closure keeps private state",
    "category": "closure_scope",
    "description": "A closure mutates an outer variable each time it executes.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int counter = 0;
            int inc() {
                counter = counter + 1;
                return counter;
            }
            writeln("inc1=", inc());
            writeln("inc2=", inc());
            return 0;
        }
    """,
    "expected_stdout": """
        inc1=1
        inc2=2
    """,
})

add({
    "id": "closure_nested_capture",
    "name": "Nested closures capture multiple levels",
    "category": "closure_scope",
    "description": "An inner closure sees both its parent's locals and outer lexical bindings.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int factor = 2;
            int outer(int n) {
                int inner(int m) {
                    return (n + m) * factor;
                }
                return inner(3);
            }
            writeln("combo=", outer(4));
            return 0;
        }
    """,
    "expected_stdout": "combo=14",
})

add({
    "id": "closure_escape_local_error",
    "name": "Escaping closure cannot capture locals",
    "category": "closure_scope",
    "description": "Returning a closure that captures a local should be rejected for safety.",
    "expect": "compile_error",
    "code": """
        int (*makeAdder(int seed))(int) {
            int base = seed;
            int inner(int delta) {
                return base + delta;
            }
            return inner;
        }
        int main() {
            int (*adder)(int) = makeAdder(5);
            return adder(1);
        }
    """,
    "expected_stderr_substring": "lifetime",
    "failure_reason": "Closures that capture locals must not escape their defining scope.",
})

add({
    "id": "closure_loop_capture_error",
    "name": "Loop capture invoked after iteration",
    "category": "closure_scope",
    "description": "Closures capturing loop indices cannot be called after the loop completes if the binding expired.",
    "expect": "compile_error",
    "code": """
        int (*saved)(void);
        int main() {
            for (int i = 0; i < 2; i = i + 1) {
                int capture(void) {
                    return i;
                }
                saved = capture;
            }
            return saved();
        }
    """,
    "expected_stderr_substring": "lifetime",
    "failure_reason": "Loop-scoped captures must not survive beyond the loop.",
})

add({
    "id": "closure_missing_capture_error",
    "name": "Closure referencing unknown name",
    "category": "closure_scope",
    "description": "Closures must fail when referencing identifiers that are not in scope.",
    "expect": "compile_error",
    "code": """
        int main() {
            int outer = 3;
            int inner() {
                return ghost + outer;
            }
            return inner();
        }
    """,
    "expected_stderr_substring": "ghost",
    "failure_reason": "Lexical resolution inside closures must match normal scope rules.",
})

add({
    "id": "closure_random_reference_capture",
    "name": "Randomised closure retains reference",
    "category": "closure_scope",
    "description": "Random identifiers and extra braces still capture the correct outer binding.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int {{outer_name}} = 5;
            int {{inner_name}}(int {{param_name}}) {
                {{outer_name}} = {{outer_name}} + {{param_name}};
                writeln("close-ok");
                return {{outer_name}};
            }
{{invoke_block}}
            writeln("final=", {{outer_name}});
            return 0;
        }
    """,
    "expected_stdout": """
        close-ok
        done 6
        final=6
    """,
    "placeholders": {
        "outer_name": {"type": "identifier", "min_length": 5},
        "inner_name": {"type": "identifier", "min_length": 5, "avoid_placeholders": ["outer_name"]},
        "param_name": {"type": "identifier", "min_length": 3, "avoid_placeholders": ["outer_name", "inner_name"]},
        "invoke_block": {
            "type": "nested_block",
            "indent": "    ",
            "min_depth": 1,
            "max_depth": 2,
            "header": "if (true)",
            "placeholders": {
                "block_comment": {"type": "comment", "words": ["closure", "random"]}
            },
            "body": [
                "writeln(\"done\", {{inner_name}}(1));"
            ],
            "trailing": "{{block_comment}}"
        }
    },
})

add({
    "id": "closure_random_escape_error",
    "name": "Randomised escaping closure rejected",
    "category": "closure_scope",
    "description": "Random identifiers still cannot allow a closure that captures locals to escape.",
    "expect": "compile_error",
    "code": """
        int (*{{maker_name}}(int seed))(int) {
            int {{captured_name}} = seed;
            int {{inner_name}}(int delta) {
                return {{captured_name}} + delta;
            }
            return {{inner_name}};
        }
        int main() {
            int (*{{alias_name}})(int) = {{maker_name}}(2);
            return {{alias_name}}(1);
        }
    """,
    "expected_stderr_substring": "lifetime",
    "failure_reason": "Escaping closures must fail regardless of identifier spelling.",
    "placeholders": {
        "maker_name": {"type": "identifier", "min_length": 5},
        "captured_name": {"type": "identifier", "min_length": 5},
        "inner_name": {"type": "identifier", "min_length": 5},
        "alias_name": {"type": "identifier", "min_length": 5},
    },
})

# ---------------------------------------------------------------------------
# Category D: Modules, packages, and imports
# ---------------------------------------------------------------------------

add({
    "id": "module_public_symbol_visible",
    "name": "Imported module exposes public symbols",
    "category": "module_scope",
    "description": "An imported module makes exported functions available to the caller.",
    "expect": "runtime_ok",
    "code": """
        #import "{{support_dir}}/modules/mod_public.rea";
        int main() {
            writeln("value=", ModPublic.getValue());
            return 0;
        }
    """,
    "expected_stdout": "value=12",
    "files": [
        {
            "path": "modules/mod_public.rea",
            "code": """
                module ModPublic {
                    export const int VALUE = 12;
                    export int getValue() {
                        return VALUE;
                    }
                    const int hiddenHelper = 99;
                }
            """
        }
    ]
})

add({
    "id": "module_alias_local_precedence",
    "name": "Local symbol beats imported alias",
    "category": "module_scope",
    "description": "A locally declared symbol hides an imported one of the same simple name.",
    "expect": "runtime_ok",
    "code": """
        #import "{{support_dir}}/modules/mod_alias_base.rea" as DataModule;
        int answer() {
            return 1;
        }
        int main() {
            writeln("local=", answer());
            writeln("qualified=", DataModule.answer());
            return 0;
        }
    """,
    "expected_stdout": """
        local=1
        qualified=42
    """,
    "files": [
        {
            "path": "modules/mod_alias_base.rea",
            "code": """
                module DataModule {
                    export int answer() {
                        return 42;
                    }
                }
            """
        }
    ]
})

add({
    "id": "module_reexport_transitive",
    "name": "Re-export forwards symbols",
    "category": "module_scope",
    "description": "Modules may re-export imports so downstream users see the forwarded names.",
    "expect": "runtime_ok",
    "code": """
        #import "{{support_dir}}/modules/layer_b.rea";
        int main() {
            writeln("forward=", LayerB.forwarded());
            return 0;
        }
    """,
    "expected_stdout": "forward=9",
    "files": [
        {
            "path": "modules/layer_a.rea",
            "code": """
                module LayerA {
                    export const int VALUE = 9;
                    export int forwarded() {
                        return VALUE;
                    }
                }
            """
        },
        {
            "path": "modules/layer_b.rea",
            "code": """
                #import "{{support_dir}}/modules/layer_a.rea";
                module LayerB {
                    export int forwarded() {
                        return LayerA.forwarded();
                    }
                }
            """
        }
    ]
})

add({
    "id": "module_private_symbol_hidden_error",
    "name": "Private module member is hidden",
    "category": "module_scope",
    "description": "Attempting to reference a non-exported module member must fail.",
    "expect": "compile_error",
    "code": """
        #import "{{support_dir}}/modules/mod_private.rea";
        int main() {
            return ModPrivate.secret;
        }
    """,
    "expected_stderr_substring": "not exported",
    "failure_reason": "Non-exported module members must remain private to the module.",
    "files": [
        {
            "path": "modules/mod_private.rea",
            "code": """
                module ModPrivate {
                    const int secret = 77;
                    export int surface() {
                        return secret;
                    }
                }
            """
        }
    ]
})

add({
    "id": "module_alias_unqualified_error",
    "name": "Alias requires qualification",
    "category": "module_scope",
    "description": "Using an aliased module member without its alias qualifier should fail.",
    "expect": "compile_error",
    "code": """
        #import "{{support_dir}}/modules/mod_util.rea" as Util;
        int main() {
            return helper();
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Aliased imports must be referenced through the chosen alias.",
    "files": [
        {
            "path": "modules/mod_util.rea",
            "code": """
                module Util {
                    export int helper() {
                        return 5;
                    }
                }
            """
        }
    ]
})

add({
    "id": "module_conflicting_import_error",
    "name": "Ambiguous import rejected",
    "category": "module_scope",
    "description": "Conflicting imported symbols with the same name must trigger an ambiguity error.",
    "expect": "compile_error",
    "code": """
        #import "{{support_dir}}/modules/mod_left.rea";
        #import "{{support_dir}}/modules/mod_right.rea";
        int main() {
            return shared();
        }
    """,
    "expected_stderr_substring": "ambiguous",
    "failure_reason": "Using an unqualified name shared by imports should be diagnosed as ambiguous.",
    "files": [
        {
            "path": "modules/mod_left.rea",
            "code": """
                module Left {
                    export int shared() {
                        return 1;
                    }
                }
            """
        },
        {
            "path": "modules/mod_right.rea",
            "code": """
                module Right {
                    export int shared() {
                        return 2;
                    }
                }
            """
        }
    ]
})

add({
    "id": "module_random_alias_pass",
    "name": "Random alias import succeeds",
    "category": "module_scope",
    "description": "Randomised module and alias names still resolve correctly with qualification.",
    "expect": "runtime_ok",
    "code": """
        #import "{{support_dir}}/modules/{{module_file}}" as {{alias_name}};
        int main() {
            writeln("module-ok", {{alias_name}}.fingerprint());
            return 0;
        }
    """,
    "expected_stdout": "module-ok 314",
    "placeholders": {
        "module_file": {"type": "literal", "value": "mod_rand.rea"},
        "alias_name": {"type": "identifier", "min_length": 5}
    },
    "files": [
        {
            "path": "modules/mod_rand.rea",
            "code": """
                module RandMod {
                    export int fingerprint() {
                        return 314;
                    }
                }
            """
        }
    ]
})

add({
    "id": "module_random_ambiguous_error",
    "name": "Random ambiguous import rejected",
    "category": "module_scope",
    "description": "Randomised conflicting import names must still be reported as ambiguous.",
    "expect": "compile_error",
    "code": """
        #import "{{support_dir}}/modules/mod_x.rea";
        #import "{{support_dir}}/modules/mod_y.rea";
        int main() {
            return {{conflict_name}}();
        }
    """,
    "expected_stderr_substring": "ambiguous",
    "failure_reason": "Ambiguity diagnostics must not depend on identifier spelling.",
    "placeholders": {
        "conflict_name": {"type": "literal", "value": "shared"}
    },
    "files": [
        {
            "path": "modules/mod_x.rea",
            "code": """
                module XMod {
                    export int shared() {
                        return 12;
                    }
                }
            """
        },
        {
            "path": "modules/mod_y.rea",
            "code": """
                module YMod {
                    export int shared() {
                        return 18;
                    }
                }
            """
        }
    ]
})

# ---------------------------------------------------------------------------
# Category E: Constants and immutability
# ---------------------------------------------------------------------------

add({
    "id": "const_global_visible_in_blocks",
    "name": "Global constant visible everywhere",
    "category": "const_scope",
    "description": "Global constants remain readable inside nested blocks without mutation.",
    "expect": "runtime_ok",
    "code": """
        const int LIMIT = 7;
        int main() {
            int total = LIMIT + 3;
            writeln("total=", total);
            return 0;
        }
    """,
    "expected_stdout": "total=10",
})

add({
    "id": "const_shadow_by_local_allowed",
    "name": "Mutable shadow of const allowed",
    "category": "const_scope",
    "description": "A local mutable binding may shadow a constant without altering the constant value.",
    "expect": "runtime_ok",
    "code": """
        const int LIMIT = 5;
        int main() {
            int globalCopy = LIMIT;
            int LIMIT = 2;
            writeln("local=", LIMIT);
            writeln("global=", globalCopy);
            return 0;
        }
    """,
    "expected_stdout": """
        local=2
        global=5
    """,
})

add({
    "id": "const_function_scope_independent",
    "name": "Function-scoped const independent",
    "category": "const_scope",
    "description": "Constants declared inside functions should not collide with outer constants.",
    "expect": "runtime_ok",
    "code": """
        const int LIMIT = 4;
        int compute() {
            const int LIMIT = 2;
            return LIMIT;
        }
        int main() {
            writeln("inner=", compute());
            writeln("outer=", LIMIT);
            return 0;
        }
    """,
    "expected_stdout": """
        inner=2
        outer=4
    """,
})

add({
    "id": "const_reassignment_error",
    "name": "Direct reassignment of const fails",
    "category": "const_scope",
    "description": "Reassigning a constant should produce an error.",
    "expect": "compile_error",
    "code": """
        const int LIMIT = 5;
        int main() {
            LIMIT = 6;
            return 0;
        }
    """,
    "expected_stderr_substring": "const",
    "failure_reason": "Constants must be immutable after initialization.",
})

add({
    "id": "const_block_reassignment_error",
    "name": "Block reassignment of const fails",
    "category": "const_scope",
    "description": "Constants remain immutable even when referenced inside nested blocks.",
    "expect": "compile_error",
    "code": """
        const int LIMIT = 5;
        int main() {
            if (true) {
                LIMIT = LIMIT + 1;
            }
            return LIMIT;
        }
    """,
    "expected_stderr_substring": "const",
    "failure_reason": "Nested blocks must respect constant immutability.",
})

add({
    "id": "const_shadow_const_mutation_error",
    "name": "Shadowed const still immutable",
    "category": "const_scope",
    "description": "Shadowing a constant with another constant keeps immutability enforcement.",
    "expect": "compile_error",
    "code": """
        int main() {
            const int LIMIT = 3;
            const int LIMIT = 4;
            LIMIT = 5;
            return LIMIT;
        }
    """,
    "expected_stderr_substring": "const",
    "failure_reason": "Constant shadow should not permit reassignment.",
})

add({
    "id": "const_class_member_initializer_uses_prior_const",
    "name": "Class const initialisers see earlier consts",
    "category": "const_scope",
    "description": "Constants declared inside a class may reference earlier constants from the same class.",
    "expect": "runtime_ok",
    "code": """
        const int GLOBAL_SHIFT = 3;

        class Layout {
            const int BaseY = 10;
            const int Height = BaseY + GLOBAL_SHIFT + 2;
            int captured;

            void Layout() {
                myself.captured = Height;
            }

            int combined() {
                return Height + BaseY;
            }
        }

        int main() {
            Layout layout = new Layout();
            writeln("captured=", layout.captured);
            writeln("combined=", layout.combined());
            return 0;
        }
    """,
    "expected_stdout": """
        captured=15
        combined=25
    """,
})

add({
    "id": "const_random_shadow_pass",
    "name": "Randomised const shadow allowed",
    "category": "const_scope",
    "description": "Random identifiers demonstrate const shadowing with unique local names.",
    "expect": "runtime_ok",
    "code": """
        const int {{const_name}} = 8;
        int main() {
            int {{local_name}} = {{const_name}} - 3;
            writeln("shadow-const=", {{const_name}});
            writeln("shadow-local=", {{local_name}});
            return 0;
        }
    """,
    "expected_stdout": """
        shadow-const=8
        shadow-local=5
    """,
    "placeholders": {
        "const_name": {"type": "identifier", "min_length": 5},
        "local_name": {"type": "identifier", "min_length": 5, "avoid_placeholders": ["const_name"]},
    },
})

add({
    "id": "const_random_assign_error",
    "name": "Randomised const reassignment rejected",
    "category": "const_scope",
    "description": "Random identifier names still trigger errors when const bindings are reassigned.",
    "expect": "compile_error",
    "code": """
        const int {{const_name}} = 3;
        int main() {
            {{const_name}} = 4;
            return {{const_name}};
        }
    """,
    "expected_stderr_substring": "const",
    "failure_reason": "Const immutability must not depend on identifier spelling.",
    "placeholders": {
        "const_name": {"type": "identifier", "min_length": 4}
    },
})

# ---------------------------------------------------------------------------
# Category F: Types and type aliases
# ---------------------------------------------------------------------------

add({
    "id": "type_alias_basic_usage",
    "name": "Type alias introduces new name",
    "category": "type_scope",
    "description": "A simple type alias can be used as a type without affecting values.",
    "expect": "runtime_ok",
    "code": """
        type alias Count = int;
        int main() {
            Count value = 5;
            writeln("count=", value);
            return 0;
        }
    """,
    "expected_stdout": "count=5",
})

add({
    "id": "type_value_identifier_independent",
    "name": "Type and value identifiers independent",
    "category": "type_scope",
    "description": "A value can reuse a type alias name without colliding with the alias itself.",
    "expect": "runtime_ok",
    "code": """
        type alias Number = int;
        int main() {
            int Number = 3;
            Number aliasValue = 4;
            writeln("alias=", aliasValue);
            writeln("shadow=", Number);
            return 0;
        }
    """,
    "expected_stdout": """
        alias=4
        shadow=3
    """,
})

add({
    "id": "type_generic_parameter_scope",
    "name": "Generic parameter scoped to definition",
    "category": "type_scope",
    "description": "Generic type parameters should be visible only inside their declaration.",
    "expect": "runtime_ok",
    "code": """
        type alias Box<T> = T;
        T unwrap<T>(T value) {
            return value;
        }
        int main() {
            writeln("boxed=", unwrap<int>(6));
            return 0;
        }
    """,
    "expected_stdout": "boxed=6",
})

add({
    "id": "type_alias_redefinition_error",
    "name": "Alias redefinition rejected",
    "category": "type_scope",
    "description": "Defining the same type alias twice in one scope must fail.",
    "expect": "compile_error",
    "code": """
        type alias Id = int;
        type alias Id = float;
        int main() {
            Id value = 1;
            return value;
        }
    """,
    "expected_stderr_substring": "type alias",
    "failure_reason": "Type aliases must be unique within their scope.",
})

add({
    "id": "type_parameter_leak_error",
    "name": "Generic parameter does not leak",
    "category": "type_scope",
    "description": "Generic parameters should not be visible outside their declaration.",
    "expect": "compile_error",
    "code": """
        type alias Wrapper<T> = T;
        Wrapper<int> makeInt() {
            return 1;
        }
        int main() {
            T leaked = makeInt();
            return leaked;
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Generic type parameters are scoped to their declaration site.",
})

add({
    "id": "type_value_space_conflict_error",
    "name": "Type name cannot shadow in same space",
    "category": "type_scope",
    "description": "Declaring a type alias that conflicts with an existing type should fail.",
    "expect": "compile_error",
    "code": """
        type alias int = float;
        int main() {
            int value = 1;
            return value;
        }
    """,
    "expected_stderr_substring": "type name",
    "failure_reason": "Built-in type names must not be re-aliased in the same namespace.",
})

add({
    "id": "type_random_alias_pass",
    "name": "Randomised alias usage works",
    "category": "type_scope",
    "description": "Random alias names should behave the same as fixed ones.",
    "expect": "runtime_ok",
    "code": """
        type alias {{alias_name}} = int;
        int main() {
            {{alias_name}} value = 11;
            writeln("alias-ok=", value);
            return 0;
        }
    """,
    "expected_stdout": "alias-ok=11",
    "placeholders": {
        "alias_name": {"type": "identifier", "min_length": 5}
    },
})

add({
    "id": "type_random_param_leak_error",
    "name": "Randomised generic leak rejected",
    "category": "type_scope",
    "description": "Generic parameter leaks must be rejected regardless of identifier spelling.",
    "expect": "compile_error",
    "code": """
        type alias Holder<{{param}}> = {{param}};
        {{param}} make();
        int main() {
            {{param}} value = 0;
            return value;
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Generic scope rules cannot depend on parameter naming.",
    "placeholders": {
        "param": {"type": "identifier", "min_length": 4}
    },
})

# ---------------------------------------------------------------------------
# Category G: Pattern bindings and exception handlers
# ---------------------------------------------------------------------------

add({
    "id": "pattern_match_scopes_binding",
    "name": "Pattern binding scoped to branch",
    "category": "pattern_scope",
    "description": "Variables bound in a match arm remain limited to that arm.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int value = 5;
            match value {
                case 5 -> {
                    int branch = value + 1;
                    writeln("branch=", branch);
                }
                default -> writeln("fallback");
            }
            writeln("original=", value);
            return 0;
        }
    """,
    "expected_stdout": """
        branch=6
        original=5
    """,
})

add({
    "id": "pattern_guard_respects_scope",
    "name": "Pattern guards see bound names",
    "category": "pattern_scope",
    "description": "Pattern guards should use the freshly bound variables without leaking them.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            match 3 {
                case x if x > 2 -> writeln("guard=", x);
                default -> writeln("miss");
            }
            return 0;
        }
    """,
    "expected_stdout": "guard=3",
})

add({
    "id": "exception_handler_scoped",
    "name": "Exception variable scoped to handler",
    "category": "pattern_scope",
    "description": "Exception handler variables must not leak outside the catch block.",
    "expect": "runtime_ok",
    "code": """
        void fail() {
            throw 7;
        }
        int main() {
            try {
                fail();
            } catch (int err) {
                writeln("caught=", err);
            }
            writeln("after try");
            return 0;
        }
    """,
    "expected_stdout": """
        caught=7
        after try
    """,
})

add({
    "id": "pattern_variable_leak_error",
    "name": "Pattern binding does not leak",
    "category": "pattern_scope",
    "description": "Referencing a pattern-bound variable outside the match arm should fail.",
    "expect": "compile_error",
    "code": """
        int main() {
            match 1 {
                case value -> writeln(value);
                default -> writeln(0);
            }
            return value;
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Pattern variables are scoped to the arm in which they are bound.",
})

add({
    "id": "exception_variable_leak_error",
    "name": "Exception variable cannot escape",
    "category": "pattern_scope",
    "description": "Exception handler identifiers should not be visible after the catch block.",
    "expect": "compile_error",
    "code": """
        int main() {
            try {
                throw 1;
            } catch (int err) {
                writeln("caught");
            }
            return err;
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Catch variables must be scoped to the handler.",
})

add({
    "id": "pattern_duplicate_binding_error",
    "name": "Duplicate pattern binding rejected",
    "category": "pattern_scope",
    "description": "Binding the same identifier twice in one pattern should raise an error.",
    "expect": "compile_error",
    "code": """
        int main() {
            match 1 {
                case (x, x) -> return x;
                default -> return 0;
            }
        }
    """,
    "expected_stderr_substring": "duplicate",
    "failure_reason": "Pattern bindings must use unique identifiers per arm.",
})

add({
    "id": "pattern_random_binding_pass",
    "name": "Random pattern binding respects scope",
    "category": "pattern_scope",
    "description": "Random identifier names in patterns still obey scoping rules.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            match 2 {
                case {{binding}} -> writeln("rand=", {{binding}} + 1);
                default -> writeln("miss");
            }
            writeln("done");
            return 0;
        }
    """,
    "expected_stdout": """
        rand=3
        done
    """,
    "placeholders": {
        "binding": {"type": "identifier", "min_length": 4}
    },
})

add({
    "id": "exception_random_leak_error",
    "name": "Random catch variable leak rejected",
    "category": "pattern_scope",
    "description": "Random identifier names in catch blocks must not leak.",
    "expect": "compile_error",
    "code": """
        int main() {
            try {
                throw 2;
            } catch (int {{catch_name}}) {
                writeln("handled");
            }
            return {{catch_name}};
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Catch scope cannot depend on identifier spelling.",
    "placeholders": {
        "catch_name": {"type": "identifier", "min_length": 5}
    },
})

# ---------------------------------------------------------------------------
# Category H: Name resolution order
# ---------------------------------------------------------------------------

add({
    "id": "name_resolution_prefers_inner",
    "name": "Inner binding wins over outer",
    "category": "resolution_scope",
    "description": "Lexical lookup should select the innermost binding first.",
    "expect": "runtime_ok",
    "code": """
        int value = 1;
        int main() {
            int globalCopy = value;
            int value = 2;
            writeln("inner=", value);
            writeln("outer=", globalCopy);
            return 0;
        }
    """,
    "expected_stdout": """
        inner=2
        outer=1
    """,
})

add({
    "id": "name_resolution_local_over_import",
    "name": "Local symbol overrides imported",
    "category": "resolution_scope",
    "description": "Local bindings must override imported names unless qualified.",
    "expect": "runtime_ok",
    "code": """
        #import "{{support_dir}}/resolution/mod_numbers.rea" as Numbers;
        int total = 42;
        int main() {
            int total = 99;
            writeln("local=", total);
            writeln("qualified=", Numbers.total());
            return 0;
        }
    """,
    "expected_stdout": """
        local=99
        qualified=42
    """,
    "files": [
        {
            "path": "resolution/mod_numbers.rea",
            "code": """
                module Numbers {
                    export int total() {
                        return 42;
                    }
                }
            """
        }
    ]
})

add({
    "id": "name_resolution_alias_required",
    "name": "Alias provides unique path",
    "category": "resolution_scope",
    "description": "Qualified references via alias should resolve even when names clash.",
    "expect": "runtime_ok",
    "code": """
        #import "{{support_dir}}/resolution/mod_alpha.rea" as Alpha;
        #import "{{support_dir}}/resolution/mod_beta.rea" as Beta;
        int main() {
            writeln("alpha=", Alpha.ident());
            writeln("beta=", Beta.ident());
            return 0;
        }
    """,
    "expected_stdout": """
        alpha=1
        beta=2
    """,
    "files": [
        {
            "path": "resolution/mod_alpha.rea",
            "code": """
                module Alpha {
                    export int ident() {
                        return 1;
                    }
                }
            """
        },
        {
            "path": "resolution/mod_beta.rea",
            "code": """
                module Beta {
                    export int ident() {
                        return 2;
                    }
                }
            """
        }
    ]
})

add({
    "id": "name_resolution_ambiguous_import_error",
    "name": "Ambiguous reference rejected",
    "category": "resolution_scope",
    "description": "Using an unqualified name that exists in multiple imports must be an error.",
    "expect": "compile_error",
    "code": """
        #import "{{support_dir}}/resolution/mod_left.rea";
        #import "{{support_dir}}/resolution/mod_right.rea";
        int main() {
            return ident();
        }
    """,
    "expected_stderr_substring": "ambiguous",
    "failure_reason": "Name resolution must report ambiguity across imports.",
    "files": [
        {
            "path": "resolution/mod_left.rea",
            "code": """
                module Left {
                    export int ident() {
                        return 3;
                    }
                }
            """
        },
        {
            "path": "resolution/mod_right.rea",
            "code": """
                module Right {
                    export int ident() {
                        return 4;
                    }
                }
            """
        }
    ]
})

add({
    "id": "name_resolution_unknown_identifier_error",
    "name": "Unknown identifier reported",
    "category": "resolution_scope",
    "description": "Referencing an undefined name must produce a clear error.",
    "expect": "compile_error",
    "code": """
        int main() {
            return missingName;
        }
    """,
    "expected_stderr_substring": "missingName",
    "failure_reason": "Resolver must flag missing identifiers.",
})

add({
    "id": "name_resolution_alias_required_error",
    "name": "Alias required for conflicting imports",
    "category": "resolution_scope",
    "description": "Referencing conflicting imports without aliasing should fail.",
    "expect": "compile_error",
    "code": """
        #import "{{support_dir}}/resolution/mod_alpha.rea";
        #import "{{support_dir}}/resolution/mod_beta.rea";
        int main() {
            return ident();
        }
    """,
    "expected_stderr_substring": "ambiguous",
    "failure_reason": "Conflicts require explicit qualification.",
})

add({
    "id": "name_resolution_random_shadow_pass",
    "name": "Random inner shadow chosen",
    "category": "resolution_scope",
    "description": "Random identifier names still prefer the innermost binding.",
    "expect": "runtime_ok",
    "code": """
        int {{outer_name}} = 10;
        int main() {
            int {{outer_name}} = 20;
            writeln("inner=", {{outer_name}});
            return 0;
        }
    """,
    "expected_stdout": "inner=20",
    "placeholders": {
        "outer_name": {"type": "identifier", "min_length": 5}
    },
})

add({
    "id": "name_resolution_random_ambiguous_error",
    "name": "Random ambiguous reference rejected",
    "category": "resolution_scope",
    "description": "Ambiguity diagnostics remain even with random names.",
    "expect": "compile_error",
    "code": """
        #import "{{support_dir}}/resolution/mod_u.rea";
        #import "{{support_dir}}/resolution/mod_v.rea";
        int main() {
            return {{shared_name}}();
        }
    """,
    "expected_stderr_substring": "ambiguous",
    "failure_reason": "Ambiguity must be reported regardless of identifier spelling.",
    "placeholders": {
        "shared_name": {"type": "literal", "value": "shared"}
    },
    "files": [
        {
            "path": "resolution/mod_u.rea",
            "code": """
                module UMod {
                    export int shared() {
                        return 8;
                    }
                }
            """
        },
        {
            "path": "resolution/mod_v.rea",
            "code": """
                module VMod {
                    export int shared() {
                        return 9;
                    }
                }
            """
        }
    ]
})

# ---------------------------------------------------------------------------
# Category I: Hoisting and forward references
# ---------------------------------------------------------------------------

add({
    "id": "hoist_function_call_before_decl",
    "name": "Functions hoisted for forward call",
    "category": "hoisting_scope",
    "description": "Functions should be callable before their definition appears.",
    "expect": "runtime_ok",
    "code": """
        int addOne(int value);
        int main() {
            writeln("result=", addOne(2));
            return 0;
        }
        int addOne(int value) {
            return value + 1;
        }
    """,
    "expected_stdout": "result=3",
})

add({
    "id": "hoist_mutual_recursion_forward",
    "name": "Mutual recursion resolves via hoisting",
    "category": "hoisting_scope",
    "description": "Mutually recursive functions declared later should still resolve.",
    "expect": "runtime_ok",
    "code": """
        bool odd(int n);
        bool even(int n) {
            if (n == 0) {
                return true;
            }
            return odd(n - 1);
        }
        bool odd(int n) {
            if (n == 0) {
                return false;
            }
            return even(n - 1);
        }
        int main() {
            writeln("even?", even(4));
            writeln("odd?", odd(5));
            return 0;
        }
    """,
    "expected_stdout": """
        even?true
        odd?true
    """,
})

add({
    "id": "hoist_procedure_forward_decl",
    "name": "Procedures hoisted before body",
    "category": "hoisting_scope",
    "description": "Procedures declared later should be callable without prototypes.",
    "expect": "runtime_ok",
    "code": """
        void logMessage();
        int main() {
            logMessage();
            return 0;
        }
        void logMessage() {
            writeln("log-ok");
        }
    """,
    "expected_stdout": "log-ok",
})

add({
    "id": "hoist_variable_use_before_decl_error",
    "name": "Variable not hoisted",
    "category": "hoisting_scope",
    "description": "Variables should not be accessible before their declaration.",
    "expect": "compile_error",
    "code": """
        int main() {
            total = 5;
            int total = 1;
            return total;
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Variables are not hoisted like functions.",
})

add({
    "id": "hoist_const_use_before_decl_error",
    "name": "Const not hoisted",
    "category": "hoisting_scope",
    "description": "Constants should not be used before they are declared.",
    "expect": "compile_error",
    "code": """
        const int LIMIT = value;
        const int value = 5;
        int main() {
            return LIMIT;
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Const initializers must only reference earlier declarations.",
})

add({
    "id": "hoist_nested_function_not_visible_error",
    "name": "Nested hoisting limited to scope",
    "category": "hoisting_scope",
    "description": "Nested functions should not be callable before they are defined in the same scope.",
    "expect": "compile_error",
    "code": """
        int main() {
            return inner();
            int inner() {
                return 2;
            }
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Nested definitions are not hoisted above their declaration site.",
})

add({
    "id": "hoist_random_function_pass",
    "name": "Random function hoist works",
    "category": "hoisting_scope",
    "description": "Random function names should still support forward calls.",
    "expect": "runtime_ok",
    "code": """
        int {{func_name}}(int v);
        int main() {
            writeln("rand=", {{func_name}}(4));
            return 0;
        }
        int {{func_name}}(int v) {
            return v + 6;
        }
    """,
    "expected_stdout": "rand=10",
    "placeholders": {
        "func_name": {"type": "identifier", "min_length": 5}
    },
})

add({
    "id": "hoist_random_variable_error",
    "name": "Random variable not hoisted",
    "category": "hoisting_scope",
    "description": "Random names should still obey non-hoisting rules for variables.",
    "expect": "compile_error",
    "code": """
        int main() {
            {{var_name}} = 1;
            int {{var_name}} = 2;
            return {{var_name}};
        }
    """,
    "expected_stderr_substring": "not in scope",
    "failure_reason": "Variable hoisting must be disallowed regardless of identifier.",
    "placeholders": {
        "var_name": {"type": "identifier", "min_length": 4}
    },
})

# ---------------------------------------------------------------------------
# Integration test combining multiple scope rules
# ---------------------------------------------------------------------------

add({
    "id": "integration_scope_closure_module_mix",
    "name": "Integration of modules, closures, and shadowing",
    "category": "integration",
    "description": "Combines module imports, closures, shadowing, and lexical lookups in one scenario.",
    "expect": "runtime_ok",
    "code": """
        #import "{{support_dir}}/integration/helpers.rea" as Helpers;
        int global = 10;
        int main() {
            int global = Helpers.baseValue();
            int outer = 1;
            int compute(int delta) {
                int inner(int step) {
                    int outer = step + delta;
                    return outer + Helpers.SHIFT;
                }
                return inner(global);
            }
            int total = compute(2);
            writeln("total=", total);
            writeln("shadowed_global=", global);
            writeln("module_shift=", Helpers.SHIFT);
            return 0;
        }
    """,
    "expected_stdout": """
        total=10
        shadowed_global=5
        module_shift=3
    """,
    "files": [
        {
            "path": "integration/helpers.rea",
            "code": """
                module Helpers {
                    export int baseValue() {
                        return 5;
                    }
                    export const int SHIFT = 3;
                }
            """
        }
    ]
})

# ---------------------------------------------------------------------------
# Emit manifest
# ---------------------------------------------------------------------------

manifest = {
    "version": 1,
    "default_extension": "rea",
    "notes": "Generated by build_manifest.py",
    "tests": tests,
}

MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
print(f"Wrote {MANIFEST_PATH} with {len(tests)} tests")
