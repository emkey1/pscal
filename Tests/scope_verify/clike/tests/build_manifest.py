from __future__ import annotations

import json
from pathlib import Path
import textwrap

ROOT = Path(__file__).resolve().parent
MANIFEST_PATH = ROOT / "manifest.json"

tests = []


DEFAULT_PREAMBLE = "#include <stdio.h>\n\n"


def normalise(text: str) -> str:
    return textwrap.dedent(text).strip("\n")


def add(test):
    item = dict(test)
    inject_stdio = item.pop("inject_stdio", True)
    code_body = normalise(test["code"])
    if inject_stdio:
        lines = code_body.splitlines()
        has_stdio = any("#include" in line and "stdio.h" in line for line in lines)
        if not has_stdio:
            code_body = DEFAULT_PREAMBLE + code_body
    item["code"] = code_body
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
            printf("outer_after=%d\n", outer);
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
            printf("total=%d\n", total);
            printf("flag=%d\n", flag);
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
            printf("loop_sum=%d, after=%d\n", sum, i);
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
                printf("hidden=%d\n", hidden);
            }
            return hidden;
        }
    """,
    "expected_stderr_substring": "undefined variable",
    "failure_reason": "Block locals must not be reachable after the block closes.",
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
                printf("loop=%d\n", i);
            }
            return i;
        }
    """,
    "expected_stderr_substring": "undefined variable",
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
            printf("after=%d\n", {{outer_name}});
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
            "header": "if (1)",
            "placeholders": {
                "shadow_name": {"type": "identifier", "min_length": 5, "avoid_placeholders": ["outer_name"]},
                "delta_val": {"type": "int", "min": 1, "max": 3},
                "end_comment": {"type": "comment", "words": ["random", "block"]},
            },
            "body": [
                "int {{shadow_name}} = {{outer_name}} + {{delta_val}};",
                "if ({{shadow_name}} > 0) {",
                "    printf(\"shadow-ok\\n\");",
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
            if (1) {
                int {{temp_name}} = 5;
                printf("temp=%d\n", {{temp_name}});
            }
            return {{temp_name}};
        }
    """,
    "expected_stderr_substring": "undefined variable",
    "failure_reason": "Identifiers introduced in conditional blocks must not be visible outside.",
    "placeholders": {
        "temp_name": {"type": "identifier", "min_length": 4},
    },
})

# ---------------------------------------------------------------------------
# Category B: Function scope
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
            printf("bump=%d\n", bump(3));
            printf("global=%d\n", total);
            return 0;
        }
    """,
    "expected_stdout": """
        bump=4
        global=7
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
            printf("sum=%d\n", evenSum(4));
            return 0;
        }
    """,
    "expected_stdout": "sum=10",
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
    "expected_stderr_substring": "undefined variable",
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
    "expected_stderr_substring": "undefined variable",
    "failure_reason": "Locals belong to their function activation only.",
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
    "id": "function_random_shadowing_ok",
    "name": "Randomised parameter shadow",
    "category": "function_scope",
    "description": "Seeded random identifiers still respect function scope and globals.",
    "expect": "runtime_ok",
    "code": """
        int {{global_name}} = 2;
        int {{func_name}}(int {{param_name}}) {
            int {{local_name}} = {{param_name}} + {{global_name}};
            printf("inner-ok\n");
            return {{param_name}} - {{global_name}} + {{local_name}} - {{local_name}};
        }
        int main() {
            printf("result=%d\n", {{func_name}}(5));
            printf("global=%d\n", {{global_name}});
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
    "expected_stderr_substring": "undefined variable",
    "failure_reason": "Function parameters are local to their defining function.",
    "placeholders": {
        "builder_name": {"type": "identifier", "min_length": 5},
        "param_name": {"type": "identifier", "min_length": 4},
    },
})

# ---------------------------------------------------------------------------
# Category C: Const scope
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
            printf("total=%d\n", total);
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
            printf("local=%d\n", LIMIT);
            printf("global=%d\n", globalCopy);
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
            printf("inner=%d\n", compute());
            printf("outer=%d\n", LIMIT);
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
            if (1) {
                LIMIT = LIMIT + 1;
            }
            return LIMIT;
        }
    """,
    "expected_stderr_substring": "const",
    "failure_reason": "Nested blocks must respect constant immutability.",
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
            printf("shadow-const=%d\n", {{const_name}});
            printf("shadow-local=%d\n", {{local_name}});
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
        "const_name": {"type": "identifier", "min_length": 4},
    },
})

# ---------------------------------------------------------------------------
# Category D: Struct and type scope
# ---------------------------------------------------------------------------

add({
    "id": "type_struct_usage_ok",
    "name": "Struct usage works across scopes",
    "category": "type_scope",
    "description": "Struct definitions are visible globally and inside functions.",
    "expect": "runtime_ok",
    "code": """
        struct Point {
            int x;
            int y;
        };
        int main() {
            struct Point p;
            p.x = 2;
            p.y = 3;
            printf("point=%d,%d\n", p.x, p.y);
            return 0;
        }
    """,
    "expected_stdout": "point=2,3",
})

add({
    "id": "type_struct_shadow_variable",
    "name": "Struct tag and variable coexist",
    "category": "type_scope",
    "description": "Value identifiers can reuse struct names without collision.",
    "expect": "runtime_ok",
    "code": """
        struct Number {
            int value;
        };
        int main() {
            struct Number Number;
            Number.value = 4;
            int Number_value = 6;
            printf("struct=%d\n", Number.value);
            printf("shadow=%d\n", Number_value);
            return 0;
        }
    """,
    "expected_stdout": """
        struct=4
        shadow=6
    """,
})

add({
    "id": "type_struct_redefinition_error",
    "name": "Struct redefinition rejected",
    "category": "type_scope",
    "description": "Defining the same struct twice must fail.",
    "expect": "compile_error",
    "code": """
        struct Data {
            int value;
        };
        struct Data {
            int value;
        };
        int main() {
            return 0;
        }
    """,
    "expected_stderr_substring": "redefinition",
    "failure_reason": "Struct declarations should not be redefined in the same scope.",
})

add({
    "id": "type_random_struct_pass",
    "name": "Randomised struct usage works",
    "category": "type_scope",
    "description": "Random struct and field names behave consistently.",
    "expect": "runtime_ok",
    "code": """
        struct {{struct_name}} {
            int {{field_name}};
        };
        int main() {
            struct {{struct_name}} item;
            item.{{field_name}} = 11;
            printf("value=%d\n", item.{{field_name}});
            return 0;
        }
    """,
    "expected_stdout": "value=11",
    "placeholders": {
        "struct_name": {"type": "identifier", "min_length": 5},
        "field_name": {"type": "identifier", "min_length": 4},
    },
})

# ---------------------------------------------------------------------------
# Category E: Import and resolution scope
# ---------------------------------------------------------------------------

add({
    "id": "import_function_visible",
    "name": "Included function is visible",
    "category": "import_scope",
    "description": "Functions declared in included headers should be callable.",
    "expect": "runtime_ok",
    "code": """
        #include "{{support_dir}}/imports/add_helper.h"

        int main() {
            printf("sum=%d\n", helper_add(2, 3));
            return 0;
        }
    """,
    "expected_stdout": "sum=5",
    "files": [
        {
            "path": "imports/add_helper.h",
            "code": """
                int helper_add(int a, int b) {
                    return a + b;
                }
            """,
        }
    ],
})

add({
    "id": "import_duplicate_definition_error",
    "name": "Duplicate include definition rejected",
    "category": "import_scope",
    "description": "Conflicting headers defining the same function should trigger an error.",
    "expect": "compile_error",
    "code": """
        #include "{{support_dir}}/imports/conflict_left.h"
        #include "{{support_dir}}/imports/conflict_right.h"

        int main() {
            return repeated();
        }
    """,
    "expected_stderr_substring": "repeated",
    "failure_reason": "Using two includes that define the same function should fail.",
    "files": [
        {
            "path": "imports/conflict_left.h",
            "code": """
                int repeated() {
                    return 1;
                }
            """,
        },
        {
            "path": "imports/conflict_right.h",
            "code": """
                int repeated() {
                    return 2;
                }
            """,
        },
    ],
})

add({
    "id": "import_random_usage_ok",
    "name": "Random include usage works",
    "category": "import_scope",
    "description": "Randomised helper names continue to resolve after inclusion.",
    "expect": "runtime_ok",
    "code": """
        #include "{{support_dir}}/imports/random_helper.h"

        int main() {
            printf("helper=%d\n", {{helper_name}}(4));
            return 0;
        }
    """,
    "expected_stdout": "helper=9",
    "placeholders": {
        "helper_name": {"type": "identifier", "min_length": 5},
    },
    "files": [
        {
            "path": "imports/random_helper.h",
            "code": """
                int {{helper_name}}(int value) {
                    return value + 5;
                }
            """,
        },
    ],
})

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
            printf("inner=%d\n", value);
            printf("outer=%d\n", globalCopy);
            return 0;
        }
    """,
    "expected_stdout": """
        inner=2
        outer=1
    """,
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
    "id": "name_resolution_import_variable_shadow",
    "name": "Local variable overrides included global",
    "category": "resolution_scope",
    "description": "Locals should shadow globals brought in via headers.",
    "expect": "runtime_ok",
    "code": """
        #include "{{support_dir}}/resolution/global_value.h"

        int main() {
            int shared = 99;
            printf("local=%d\n", shared);
            printf("global=%d\n", global_shared);
            return 0;
        }
    """,
    "expected_stdout": """
        local=99
        global=7
    """,
    "files": [
        {
            "path": "resolution/global_value.h",
            "code": """
                int global_shared = 7;
            """,
        }
    ],
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
            printf("inner=%d\n", {{outer_name}});
            return 0;
        }
    """,
    "expected_stdout": "inner=20",
    "placeholders": {
        "outer_name": {"type": "identifier", "min_length": 5},
    },
})

# ---------------------------------------------------------------------------
# Category F: Hoisting scope
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
            printf("result=%d\n", addOne(2));
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
        int odd(int n);
        int even(int n) {
            if (n == 0) {
                return 1;
            }
            return odd(n - 1);
        }
        int odd(int n) {
            if (n == 0) {
                return 0;
            }
            return even(n - 1);
        }
        int main() {
            printf("even?%d\n", even(4));
            printf("odd?%d\n", odd(5));
            return 0;
        }
    """,
    "expected_stdout": """
        even?1
        odd?1
    """,
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
    "expected_stderr_substring": "undefined variable",
    "failure_reason": "Variables are not hoisted like functions.",
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
    "expected_stderr_substring": "undefined variable",
    "failure_reason": "Variable hoisting must be disallowed regardless of identifier.",
    "placeholders": {
        "var_name": {"type": "identifier", "min_length": 4},
    },
})

# ---------------------------------------------------------------------------
# Category G: Integration
# ---------------------------------------------------------------------------

add({
    "id": "integration_scope_import_shadow_mix",
    "name": "Integration of includes and shadowing",
    "category": "integration",
    "description": "Combines headers, nested blocks, and shadowing in one scenario.",
    "expect": "runtime_ok",
    "code": """
        #include "{{support_dir}}/integration/helpers.h"

        int global = 10;

        int main() {
            int global = helper_value();
            int outer = 1;
            if (global > 0) {
                int outer = helper_shift() + global;
                printf("inner=%d\n", outer);
            }
            printf("shadowed_global=%d\n", global);
            printf("module_shift=%d\n", helper_shift());
            return 0;
        }
    """,
    "expected_stdout": """
        inner=8
        shadowed_global=5
        module_shift=3
    """,
    "files": [
        {
            "path": "integration/helpers.h",
            "code": """
                int helper_value() {
                    return 5;
                }
                int helper_shift() {
                    return 3;
                }
            """,
        }
    ],
})

add({
    "id": "thread_wrappers_spawn_queue",
    "name": "Thread helpers expose VM worker controls",
    "category": "integration",
    "description": "thread_spawn_named and thread_pool_submit forward arguments, names, and expose stats.",
    "expect": "runtime_ok",
    "code": """
        int main() {
            int named = thread_spawn_named("delay", "clike_worker", 5);
            WaitForThread(named);
            int named_ok = thread_get_status(named, 1);

            int pooled = thread_pool_submit("delay", "clike_pool", 5);
            WaitForThread(pooled);
            int lookup = thread_lookup("clike_pool");
            int pooled_ok = thread_get_status(pooled, 1);
            int lookup_match = (lookup == pooled) ? 1 : 0;
            int stats_len = length(thread_stats());

            printf("named_status=%d\n", named_ok);
            printf("pooled_status=%d lookup_match=%d stats=%d\n", pooled_ok, lookup_match, stats_len);
            return 0;
        }
    """,
    "expected_stdout": """
        named_status=1
        pooled_status=1 lookup_match=1 stats=1
    """,
})


def main() -> None:
    manifest = {
        "version": 1,
        "default_extension": "cl",
        "notes": "Generated by build_manifest.py",
        "tests": tests,
    }
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
