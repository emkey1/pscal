#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(python3 -c 'import os,sys; print(os.path.realpath(os.path.dirname(sys.argv[1])))' "${BASH_SOURCE[0]}")"
ROOT_DIR="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$SCRIPT_DIR/..")"
AETHER_BIN="$ROOT_DIR/build/bin/aether"
SMOKE_FIXTURE="$ROOT_DIR/Tests/aether/smoke.aether"
CONTRACT_PASS_FIXTURE="$ROOT_DIR/Tests/aether/contracts_pass.aether"
CONTRACT_LAYOUT_PASS_FIXTURE="$ROOT_DIR/Tests/aether/contract_layout_pass.aether"
COST_PASS_FIXTURE="$ROOT_DIR/Tests/aether/cost_annotation_pass.aether"
COST_ZERO_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/cost_annotation_zero_fail.aether"
COST_UNIT_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/cost_annotation_unit_fail.aether"
COST_DETACHED_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/cost_annotation_detached_fail.aether"
COST_DUPLICATE_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/cost_annotation_duplicate_fail.aether"
CONTRACT_PRE_EMPTY_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/contract_annotation_pre_empty_fail.aether"
CONTRACT_POST_DETACHED_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/contract_annotation_post_detached_fail.aether"
CONTRACT_PURE_TRAILING_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/contract_annotation_pure_trailing_fail.aether"
CONTRACT_FAIL_PRE_FIXTURE="$ROOT_DIR/Tests/aether/contracts_fail_pre.aether"
CONTRACT_FAIL_POST_FIXTURE="$ROOT_DIR/Tests/aether/contracts_fail_post.aether"
EFFECTS_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/effects_fail_outside_fx.aether"
PRINT_ALIAS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/print_alias_pass.aether"
PRINT_ALIAS_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/print_alias_fail_outside_fx.aether"
TASK_HELPERS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/task_helpers_pass.aether"
TASK_ALIAS_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/task_alias_fail_outside_fx.aether"
HAS_BUILTIN_ALIAS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/has_builtin_alias_pass.aether"
AI_HELPERS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/ai_helpers_pass.aether"
AI_ALIAS_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/ai_alias_fail_outside_fx.aether"
RUNTIME_LINE_MAPPING_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/runtime_line_mapping_fail.aether"
INFERRED_BINDINGS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/inferred_bindings_pass.aether"
INFERRED_CONST_PASS_FIXTURE="$ROOT_DIR/Tests/aether/inferred_const_pass.aether"
INFERRED_LET_UNKNOWN_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/inferred_let_unknown_fail.aether"
DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/diagnostic_line_mapping_fail.aether"
FUNCTION_INFERENCE_SUPPORT_FIXTURE="$ROOT_DIR/Tests/aether/function_inference_support"
FUNCTION_RETURN_INFERENCE_PASS_FIXTURE="$ROOT_DIR/Tests/aether/function_return_inference_pass.aether"
OBJECT_INFERENCE_PASS_FIXTURE="$ROOT_DIR/Tests/aether/object_inference_pass.aether"
PURE_PASS_FIXTURE="$ROOT_DIR/Tests/aether/pure_pass.aether"
PURE_FAIL_EFFECTFUL_FIXTURE="$ROOT_DIR/Tests/aether/pure_fail_effectful.aether"
PURE_FAIL_NON_PURE_CALL_FIXTURE="$ROOT_DIR/Tests/aether/pure_fail_non_pure_call.aether"
PAR_PASS_FIXTURE="$ROOT_DIR/Tests/aether/par_pass.aether"
PAR_FAIL_NON_CALL_FIXTURE="$ROOT_DIR/Tests/aether/par_fail_non_call.aether"
FOR_RANGE_PASS_FIXTURE="$ROOT_DIR/Tests/aether/for_range_pass.aether"
LOOP_FORMS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/loop_forms_pass.aether"
MODULE_IMPORT_PASS_FIXTURE="$ROOT_DIR/Tests/aether/module_import_pass.aether"
MODULE_SUPPORT_FIXTURE="$ROOT_DIR/Tests/aether/module_math"
MODULE_CONST_SUPPORT_FIXTURE="$ROOT_DIR/Tests/aether/module_consts"
MODULE_CONST_IMPORT_PASS_FIXTURE="$ROOT_DIR/Tests/aether/module_const_import_pass.aether"
TOON_BLOCK_PASS_FIXTURE="$ROOT_DIR/Tests/aether/toon_block_pass.aether"
TYPE_BLOCK_PASS_FIXTURE="$ROOT_DIR/Tests/aether/type_block_pass.aether"
TYPE_INIT_PASS_FIXTURE="$ROOT_DIR/Tests/aether/type_init_pass.aether"
SELF_ALIAS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/self_alias_pass.aether"
SELF_MUTATION_PASS_FIXTURE="$ROOT_DIR/Tests/aether/self_mutation_pass.aether"
TOON_JSON_HELPERS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/toon_json_helpers_pass.aether"
TOON_HANDLE_HELPERS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/toon_handle_helpers_pass.aether"
TOON_VARIABLE_PARSE_PASS_FIXTURE="$ROOT_DIR/Tests/aether/toon_variable_parse_pass.aether"
HAS_TOON_ALIAS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/has_toon_alias_pass.aether"
TOON_HANDLE_ARITH_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_handle_arithmetic_fail.aether"
TOON_HANDLE_CROSS_ASSIGN_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_handle_cross_assign_fail.aether"
TOON_HANDLE_KIND_DOC_AS_NODE_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_handle_kind_fail_doc_as_node.aether"
TOON_HANDLE_KIND_NODE_AS_DOC_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_handle_kind_fail_node_as_doc.aether"
TOON_HANDLE_DECL_FAIL_DOC_TYPE_FIXTURE="$ROOT_DIR/Tests/aether/toon_handle_decl_fail_doc_type.aether"
TOON_HANDLE_DECL_FAIL_NODE_TYPE_FIXTURE="$ROOT_DIR/Tests/aether/toon_handle_decl_fail_node_type.aether"
TOON_HANDLE_REASSIGN_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_handle_reassign_fail.aether"
TOON_SCALAR_DECL_FAIL_TEXT_TYPE_FIXTURE="$ROOT_DIR/Tests/aether/toon_scalar_decl_fail_text_type.aether"
TOON_SCALAR_REASSIGN_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_scalar_reassign_fail.aether"
TOON_SCALAR_CROSS_ASSIGN_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_scalar_cross_assign_fail.aether"
TOON_KEY_ARG_TYPE_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_key_arg_type_fail.aether"
TOON_OBJECT_KEY_ARG_TYPE_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_object_key_arg_type_fail.aether"
TOON_INDEX_ARG_TYPE_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_index_arg_type_fail.aether"
TOON_PARSE_ARG_TYPE_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_parse_arg_type_fail.aether"
TOON_PARSE_FILE_ARG_TYPE_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_parse_file_arg_type_fail.aether"
TOON_SHAPE_SCALAR_DECL_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_shape_scalar_decl_fail.aether"
TOON_REAL_DECL_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_real_decl_fail.aether"
TOON_TYPE_DECL_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_type_decl_fail.aether"
TOON_PRESENCE_DECL_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_presence_decl_fail.aether"
TOON_DEFAULTS_DECL_FAIL_FIXTURE="$ROOT_DIR/Tests/aether/toon_defaults_decl_fail.aether"
TOON_COMMENT_ARITH_PASS_FIXTURE="$ROOT_DIR/Tests/aether/toon_comment_arithmetic_pass.aether"
TOON_NESTED_HELPERS_PASS_FIXTURE="$ROOT_DIR/Tests/aether/toon_nested_helpers_pass.aether"
SHOWCASE_EXAMPLE="$ROOT_DIR/Examples/aether/showcase/agent_report"

if [ ! -x "$AETHER_BIN" ]; then
    echo "missing aether binary: $AETHER_BIN" >&2
    exit 1
fi

for fixture in \
    "$SMOKE_FIXTURE" \
    "$CONTRACT_PASS_FIXTURE" \
    "$CONTRACT_LAYOUT_PASS_FIXTURE" \
    "$COST_PASS_FIXTURE" \
    "$COST_ZERO_FAIL_FIXTURE" \
    "$COST_UNIT_FAIL_FIXTURE" \
    "$COST_DETACHED_FAIL_FIXTURE" \
    "$COST_DUPLICATE_FAIL_FIXTURE" \
    "$CONTRACT_PRE_EMPTY_FAIL_FIXTURE" \
    "$CONTRACT_POST_DETACHED_FAIL_FIXTURE" \
    "$CONTRACT_PURE_TRAILING_FAIL_FIXTURE" \
    "$CONTRACT_FAIL_PRE_FIXTURE" \
    "$CONTRACT_FAIL_POST_FIXTURE" \
    "$EFFECTS_FAIL_FIXTURE" \
    "$PRINT_ALIAS_PASS_FIXTURE" \
    "$PRINT_ALIAS_FAIL_FIXTURE" \
    "$TASK_HELPERS_PASS_FIXTURE" \
    "$TASK_ALIAS_FAIL_FIXTURE" \
    "$HAS_BUILTIN_ALIAS_PASS_FIXTURE" \
    "$AI_HELPERS_PASS_FIXTURE" \
    "$AI_ALIAS_FAIL_FIXTURE" \
    "$RUNTIME_LINE_MAPPING_FAIL_FIXTURE" \
    "$INFERRED_BINDINGS_PASS_FIXTURE" \
    "$INFERRED_CONST_PASS_FIXTURE" \
    "$INFERRED_LET_UNKNOWN_FAIL_FIXTURE" \
    "$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE" \
    "$FUNCTION_INFERENCE_SUPPORT_FIXTURE" \
    "$FUNCTION_RETURN_INFERENCE_PASS_FIXTURE" \
    "$OBJECT_INFERENCE_PASS_FIXTURE" \
    "$PURE_PASS_FIXTURE" \
    "$PURE_FAIL_EFFECTFUL_FIXTURE" \
    "$PURE_FAIL_NON_PURE_CALL_FIXTURE" \
    "$PAR_PASS_FIXTURE" \
    "$PAR_FAIL_NON_CALL_FIXTURE" \
    "$FOR_RANGE_PASS_FIXTURE" \
    "$LOOP_FORMS_PASS_FIXTURE" \
    "$MODULE_IMPORT_PASS_FIXTURE" \
    "$MODULE_SUPPORT_FIXTURE" \
    "$MODULE_CONST_SUPPORT_FIXTURE" \
    "$MODULE_CONST_IMPORT_PASS_FIXTURE" \
    "$TOON_BLOCK_PASS_FIXTURE" \
    "$TYPE_BLOCK_PASS_FIXTURE" \
    "$TYPE_INIT_PASS_FIXTURE" \
    "$SELF_ALIAS_PASS_FIXTURE" \
    "$SELF_MUTATION_PASS_FIXTURE" \
    "$TOON_JSON_HELPERS_PASS_FIXTURE" \
    "$TOON_HANDLE_HELPERS_PASS_FIXTURE" \
    "$TOON_VARIABLE_PARSE_PASS_FIXTURE" \
    "$HAS_TOON_ALIAS_PASS_FIXTURE" \
    "$TOON_HANDLE_ARITH_FAIL_FIXTURE" \
    "$TOON_HANDLE_CROSS_ASSIGN_FAIL_FIXTURE" \
    "$TOON_HANDLE_KIND_DOC_AS_NODE_FAIL_FIXTURE" \
    "$TOON_HANDLE_KIND_NODE_AS_DOC_FAIL_FIXTURE" \
    "$TOON_HANDLE_DECL_FAIL_DOC_TYPE_FIXTURE" \
    "$TOON_HANDLE_DECL_FAIL_NODE_TYPE_FIXTURE" \
    "$TOON_HANDLE_REASSIGN_FAIL_FIXTURE" \
    "$TOON_SCALAR_DECL_FAIL_TEXT_TYPE_FIXTURE" \
    "$TOON_SCALAR_REASSIGN_FAIL_FIXTURE" \
    "$TOON_SCALAR_CROSS_ASSIGN_FAIL_FIXTURE" \
    "$TOON_KEY_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_OBJECT_KEY_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_INDEX_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_PARSE_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_PARSE_FILE_ARG_TYPE_FAIL_FIXTURE" \
    "$TOON_SHAPE_SCALAR_DECL_FAIL_FIXTURE" \
    "$TOON_REAL_DECL_FAIL_FIXTURE" \
    "$TOON_TYPE_DECL_FAIL_FIXTURE" \
    "$TOON_PRESENCE_DECL_FAIL_FIXTURE" \
    "$TOON_DEFAULTS_DECL_FAIL_FIXTURE" \
    "$TOON_COMMENT_ARITH_PASS_FIXTURE" \
    "$TOON_NESTED_HELPERS_PASS_FIXTURE" \
    "$SHOWCASE_EXAMPLE"
do
    if [ ! -f "$fixture" ]; then
        echo "missing fixture: $fixture" >&2
        exit 1
    fi
done

"$AETHER_BIN" --no-cache --no-run "$SMOKE_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache --dump-ast-json "$SMOKE_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache "$CONTRACT_PASS_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache "$CONTRACT_LAYOUT_PASS_FIXTURE" >/tmp/aether_contract_layout_pass.out
if ! grep -qx "42" /tmp/aether_contract_layout_pass.out; then
    echo "unexpected contract layout output" >&2
    cat /tmp/aether_contract_layout_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$COST_PASS_FIXTURE" >/tmp/aether_cost_pass.out
if ! grep -qx "42" /tmp/aether_cost_pass.out; then
    echo "unexpected cost annotation output" >&2
    cat /tmp/aether_cost_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$PRINT_ALIAS_PASS_FIXTURE" >/tmp/aether_print_alias_pass.out
printf 'Aether print aliases\n' >/tmp/aether_print_alias_expected.out
if ! cmp -s /tmp/aether_print_alias_expected.out /tmp/aether_print_alias_pass.out; then
    echo "unexpected print alias output" >&2
    cat /tmp/aether_print_alias_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TASK_HELPERS_PASS_FIXTURE" >/tmp/aether_task_helpers_pass.out
if ! grep -q '^named_ok = true$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper named output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
if ! grep -q '^pooled_ok = true$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper pooled output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
if ! grep -q '^lookup_match = true$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper lookup output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
if ! grep -Eq '^stats = [0-9]+$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper stats output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
if ! grep -Eq '^has_ai = (true|false)$' /tmp/aether_task_helpers_pass.out; then
    echo "unexpected task helper has_ai output" >&2
    cat /tmp/aether_task_helpers_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$HAS_BUILTIN_ALIAS_PASS_FIXTURE" >/tmp/aether_has_builtin_alias_pass.out
if ! grep -Eq '^(true|false)$' /tmp/aether_has_builtin_alias_pass.out; then
    echo "unexpected has_builtin alias output" >&2
    cat /tmp/aether_has_builtin_alias_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$AI_HELPERS_PASS_FIXTURE" >/tmp/aether_ai_helpers_pass.out
if ! grep -Eq '^has_ai = (true|false)$' /tmp/aether_ai_helpers_pass.out; then
    echo "unexpected ai helper capability output" >&2
    cat /tmp/aether_ai_helpers_pass.out >&2
    exit 1
fi
if ! grep -Eq '^has_openai = (true|false)$' /tmp/aether_ai_helpers_pass.out; then
    echo "unexpected ai helper builtin output" >&2
    cat /tmp/aether_ai_helpers_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$INFERRED_BINDINGS_PASS_FIXTURE" >/tmp/aether_inferred_bindings_pass.out
if grep -qx "yyjson unavailable" /tmp/aether_inferred_bindings_pass.out; then
    :
else
    printf 'Aether\n42\n3.500000\ntrue\n2\n' >/tmp/aether_inferred_bindings_expected.out
    if ! cmp -s /tmp/aether_inferred_bindings_expected.out /tmp/aether_inferred_bindings_pass.out; then
        echo "unexpected inferred binding output" >&2
        cat /tmp/aether_inferred_bindings_pass.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$INFERRED_CONST_PASS_FIXTURE" >/tmp/aether_inferred_const_pass.out
if ! grep -qx "Aether" /tmp/aether_inferred_const_pass.out; then
    echo "unexpected inferred const output" >&2
    cat /tmp/aether_inferred_const_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$FUNCTION_RETURN_INFERENCE_PASS_FIXTURE" >/tmp/aether_function_return_inference_pass.out
printf 'Aether\n42\n' >/tmp/aether_function_return_inference_expected.out
if ! cmp -s /tmp/aether_function_return_inference_expected.out /tmp/aether_function_return_inference_pass.out; then
    echo "unexpected function return inference output" >&2
    cat /tmp/aether_function_return_inference_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$OBJECT_INFERENCE_PASS_FIXTURE" >/tmp/aether_object_inference_pass.out
printf '42\ntrue\n' >/tmp/aether_object_inference_expected.out
if ! cmp -s /tmp/aether_object_inference_expected.out /tmp/aether_object_inference_pass.out; then
    echo "unexpected object inference output" >&2
    cat /tmp/aether_object_inference_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$PURE_PASS_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache --no-run "$PAR_PASS_FIXTURE" >/dev/null
"$AETHER_BIN" --no-cache "$FOR_RANGE_PASS_FIXTURE" >/tmp/aether_for_range_pass.out
if ! grep -qx "10" /tmp/aether_for_range_pass.out; then
    echo "unexpected for-range output" >&2
    cat /tmp/aether_for_range_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$LOOP_FORMS_PASS_FIXTURE" >/tmp/aether_loop_forms_pass.out
printf 'total = 9\nspins = 2\n' >/tmp/aether_loop_forms_expected.out
if ! cmp -s /tmp/aether_loop_forms_expected.out /tmp/aether_loop_forms_pass.out; then
    echo "unexpected loop forms output" >&2
    cat /tmp/aether_loop_forms_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$MODULE_IMPORT_PASS_FIXTURE" >/tmp/aether_module_import_pass.out
if ! grep -qx "42" /tmp/aether_module_import_pass.out; then
    echo "unexpected module import output" >&2
    cat /tmp/aether_module_import_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$MODULE_CONST_IMPORT_PASS_FIXTURE" >/tmp/aether_module_const_import_pass.out
printf 'Aether\n42\n' >/tmp/aether_module_const_import_expected.out
if ! cmp -s /tmp/aether_module_const_import_expected.out /tmp/aether_module_const_import_pass.out; then
    echo "unexpected module const import output" >&2
    cat /tmp/aether_module_const_import_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$SHOWCASE_EXAMPLE" >/tmp/aether_showcase_example.out
if grep -qx "yyjson unavailable" /tmp/aether_showcase_example.out; then
    :
else
    printf 'job 0: planner / ready / 95\njob 1: writer / review / 81\njob 2: tester / review / 72\njob 3: auditor / blocked / 55\ntotal = 4\nready = 1\nreview = 2\nblocked = 1\n' >/tmp/aether_showcase_example_expected.out
    if ! cmp -s /tmp/aether_showcase_example_expected.out /tmp/aether_showcase_example.out; then
        echo "unexpected Aether showcase output" >&2
        cat /tmp/aether_showcase_example.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$TOON_BLOCK_PASS_FIXTURE" >/tmp/aether_toon_block_pass.out
printf 'users[2]{id,name,role}:\n  1,Ada,admin\n  2,Bob,user\n' >/tmp/aether_toon_block_expected.out
if ! cmp -s /tmp/aether_toon_block_expected.out /tmp/aether_toon_block_pass.out; then
    echo "unexpected TOON block output" >&2
    cat /tmp/aether_toon_block_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TYPE_BLOCK_PASS_FIXTURE" >/tmp/aether_type_block_pass.out
if ! grep -qx "42" /tmp/aether_type_block_pass.out; then
    echo "unexpected type block output" >&2
    cat /tmp/aether_type_block_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TYPE_INIT_PASS_FIXTURE" >/tmp/aether_type_init_pass.out
if ! grep -qx "42" /tmp/aether_type_init_pass.out; then
    echo "unexpected type init output" >&2
    cat /tmp/aether_type_init_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$SELF_ALIAS_PASS_FIXTURE" >/tmp/aether_self_alias_pass.out
printf '41\n42\n' >/tmp/aether_self_alias_expected.out
if ! cmp -s /tmp/aether_self_alias_expected.out /tmp/aether_self_alias_pass.out; then
    echo "unexpected self alias output" >&2
    cat /tmp/aether_self_alias_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$SELF_MUTATION_PASS_FIXTURE" >/tmp/aether_self_mutation_pass.out
if ! grep -qx "42" /tmp/aether_self_mutation_pass.out; then
    echo "unexpected self mutation output" >&2
    cat /tmp/aether_self_mutation_pass.out >&2
    exit 1
fi
"$AETHER_BIN" --no-cache "$TOON_JSON_HELPERS_PASS_FIXTURE" >/tmp/aether_toon_json_helpers_pass.out
if grep -qx "yyjson unavailable" /tmp/aether_toon_json_helpers_pass.out; then
    :
else
    printf 'Rea\n3\n1\n' >/tmp/aether_toon_json_helpers_expected.out
    if ! cmp -s /tmp/aether_toon_json_helpers_expected.out /tmp/aether_toon_json_helpers_pass.out; then
        echo "unexpected TOON helper output" >&2
        cat /tmp/aether_toon_json_helpers_pass.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$TOON_HANDLE_HELPERS_PASS_FIXTURE" >/tmp/aether_toon_handle_helpers_pass.out
if grep -qx "yyjson unavailable" /tmp/aether_toon_handle_helpers_pass.out; then
    :
else
    printf '2\nBob\n2\n' >/tmp/aether_toon_handle_helpers_expected.out
    if ! cmp -s /tmp/aether_toon_handle_helpers_expected.out /tmp/aether_toon_handle_helpers_pass.out; then
        echo "unexpected TOON handle helper output" >&2
        cat /tmp/aether_toon_handle_helpers_pass.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$TOON_VARIABLE_PARSE_PASS_FIXTURE" >/tmp/aether_toon_variable_parse_pass.out
if grep -qx "yyjson unavailable" /tmp/aether_toon_variable_parse_pass.out; then
    :
else
    printf 'Aether\n42\n' >/tmp/aether_toon_variable_parse_expected.out
    if ! cmp -s /tmp/aether_toon_variable_parse_expected.out /tmp/aether_toon_variable_parse_pass.out; then
        echo "unexpected TOON variable parse output" >&2
        cat /tmp/aether_toon_variable_parse_pass.out >&2
        exit 1
    fi
fi
"$AETHER_BIN" --no-cache "$HAS_TOON_ALIAS_PASS_FIXTURE" >/tmp/aether_has_toon_alias_pass.out
if ! grep -Eq '^(toon-ready|toon-missing)$' /tmp/aether_has_toon_alias_pass.out; then
    echo "unexpected has_toon alias output" >&2
    cat /tmp/aether_has_toon_alias_pass.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_ARITH_FAIL_FIXTURE" >/tmp/aether_toon_handle_arith_fail.out 2>&1; then
    echo "expected TOON handle arithmetic failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "opaque TOON handle 'doc' cannot be used in arithmetic expressions" /tmp/aether_toon_handle_arith_fail.out; then
    echo "missing TOON handle arithmetic failure message" >&2
    cat /tmp/aether_toon_handle_arith_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_CROSS_ASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_handle_cross_assign_fail.out 2>&1; then
    echo "expected TOON handle cross-assignment failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "cannot assign ToonDoc handle 'doc' to ToonNode binding" /tmp/aether_toon_handle_cross_assign_fail.out; then
    echo "missing TOON handle cross-assignment failure message" >&2
    cat /tmp/aether_toon_handle_cross_assign_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_KIND_DOC_AS_NODE_FAIL_FIXTURE" >/tmp/aether_toon_handle_kind_doc_as_node_fail.out 2>&1; then
    echo "expected TOON doc-as-node failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_text_value' expects a ToonNode handle, but 'doc' is ToonDoc" /tmp/aether_toon_handle_kind_doc_as_node_fail.out; then
    echo "missing TOON doc-as-node failure message" >&2
    cat /tmp/aether_toon_handle_kind_doc_as_node_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_KIND_NODE_AS_DOC_FAIL_FIXTURE" >/tmp/aether_toon_handle_kind_node_as_doc_fail.out 2>&1; then
    echo "expected TOON node-as-doc failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_close' expects a ToonDoc handle, but 'root' is ToonNode" /tmp/aether_toon_handle_kind_node_as_doc_fail.out; then
    echo "missing TOON node-as-doc failure message" >&2
    cat /tmp/aether_toon_handle_kind_node_as_doc_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_DECL_FAIL_DOC_TYPE_FIXTURE" >/tmp/aether_toon_handle_decl_fail_doc_type.out 2>&1; then
    echo "expected TOON doc declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'doc' must use ToonDoc when initialized from 'toon_parse'" /tmp/aether_toon_handle_decl_fail_doc_type.out; then
    echo "missing TOON doc declaration type failure message" >&2
    cat /tmp/aether_toon_handle_decl_fail_doc_type.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_DECL_FAIL_NODE_TYPE_FIXTURE" >/tmp/aether_toon_handle_decl_fail_node_type.out 2>&1; then
    echo "expected TOON node declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'root' must use ToonNode when initialized from 'toon_root'" /tmp/aether_toon_handle_decl_fail_node_type.out; then
    echo "missing TOON node declaration type failure message" >&2
    cat /tmp/aether_toon_handle_decl_fail_node_type.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_HANDLE_REASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_handle_reassign_fail.out 2>&1; then
    echo "expected TOON handle reassignment failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'current' must use ToonDoc when initialized from 'toon_parse'" /tmp/aether_toon_handle_reassign_fail.out; then
    echo "missing TOON handle reassignment failure message" >&2
    cat /tmp/aether_toon_handle_reassign_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_SCALAR_DECL_FAIL_TEXT_TYPE_FIXTURE" >/tmp/aether_toon_scalar_decl_fail_text_type.out 2>&1; then
    echo "expected TOON scalar declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrong' must use Text when initialized from 'toon_get_text'" /tmp/aether_toon_scalar_decl_fail_text_type.out; then
    echo "missing TOON scalar declaration type failure message" >&2
    cat /tmp/aether_toon_scalar_decl_fail_text_type.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_SCALAR_REASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_scalar_reassign_fail.out 2>&1; then
    echo "expected TOON scalar reassignment type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'enabled' must use Text when initialized from 'toon_get_text'" /tmp/aether_toon_scalar_reassign_fail.out; then
    echo "missing TOON scalar reassignment type failure message" >&2
    cat /tmp/aether_toon_scalar_reassign_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_SCALAR_CROSS_ASSIGN_FAIL_FIXTURE" >/tmp/aether_toon_scalar_cross_assign_fail.out 2>&1; then
    echo "expected TOON scalar cross-assignment failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "cannot assign Text binding 'name' to Bool binding 'enabled'" /tmp/aether_toon_scalar_cross_assign_fail.out; then
    echo "missing TOON scalar cross-assignment failure message" >&2
    cat /tmp/aether_toon_scalar_cross_assign_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_KEY_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_key_arg_type_fail.out 2>&1; then
    echo "expected TOON key argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_get_text' expects a Text second argument, but 'badKey' is Int" /tmp/aether_toon_key_arg_type_fail.out; then
    echo "missing TOON key argument type failure message" >&2
    cat /tmp/aether_toon_key_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_OBJECT_KEY_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_object_key_arg_type_fail.out 2>&1; then
    echo "expected TOON object-key argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_key' expects a Text second argument, but 'badKey' is Bool" /tmp/aether_toon_object_key_arg_type_fail.out; then
    echo "missing TOON object-key argument type failure message" >&2
    cat /tmp/aether_toon_object_key_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_INDEX_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_index_arg_type_fail.out 2>&1; then
    echo "expected TOON index argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_at' expects a Int second argument, but 'badIndex' is Text" /tmp/aether_toon_index_arg_type_fail.out; then
    echo "missing TOON index argument type failure message" >&2
    cat /tmp/aether_toon_index_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_PARSE_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_parse_arg_type_fail.out 2>&1; then
    echo "expected TOON parse argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_parse' expects a Text or TOON first argument, but 'badPayload' is Int" /tmp/aether_toon_parse_arg_type_fail.out; then
    echo "missing TOON parse argument type failure message" >&2
    cat /tmp/aether_toon_parse_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_PARSE_FILE_ARG_TYPE_FAIL_FIXTURE" >/tmp/aether_toon_parse_file_arg_type_fail.out 2>&1; then
    echo "expected TOON parse_file argument type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_parse_file' expects a Text first argument, but 'badPath' is Bool" /tmp/aether_toon_parse_file_arg_type_fail.out; then
    echo "missing TOON parse_file argument type failure message" >&2
    cat /tmp/aether_toon_parse_file_arg_type_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_SHAPE_SCALAR_DECL_FAIL_FIXTURE" >/tmp/aether_toon_shape_scalar_decl_fail.out 2>&1; then
    echo "expected TOON shape scalar declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrongLen' must use Int when initialized from 'toon_len'" /tmp/aether_toon_shape_scalar_decl_fail.out; then
    echo "missing TOON length declaration type failure message" >&2
    cat /tmp/aether_toon_shape_scalar_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_REAL_DECL_FAIL_FIXTURE" >/tmp/aether_toon_real_decl_fail.out 2>&1; then
    echo "expected TOON real declaration type failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrong' must use Real when initialized from 'toon_get_real'" /tmp/aether_toon_real_decl_fail.out; then
    echo "missing TOON real declaration type failure message" >&2
    cat /tmp/aether_toon_real_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_TYPE_DECL_FAIL_FIXTURE" >/tmp/aether_toon_type_decl_fail.out 2>&1; then
    echo "expected TOON type inspection declaration failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrongType' must use Text when initialized from 'toon_type'" /tmp/aether_toon_type_decl_fail.out; then
    echo "missing TOON type() declaration type failure message" >&2
    cat /tmp/aether_toon_type_decl_fail.out >&2
    exit 1
fi
if ! grep -q "binding for 'wrongArr' must use Bool when initialized from 'toon_is_arr'" /tmp/aether_toon_type_decl_fail.out; then
    echo "missing TOON is_arr declaration type failure message" >&2
    cat /tmp/aether_toon_type_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_PRESENCE_DECL_FAIL_FIXTURE" >/tmp/aether_toon_presence_decl_fail.out 2>&1; then
    echo "expected TOON presence declaration failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "binding for 'wrongKey' must use Bool when initialized from 'toon_has_key'" /tmp/aether_toon_presence_decl_fail.out; then
    echo "missing TOON has_key declaration type failure message" >&2
    cat /tmp/aether_toon_presence_decl_fail.out >&2
    exit 1
fi
if ! grep -q "binding for 'wrongIndex' must use Bool when initialized from 'toon_has_at'" /tmp/aether_toon_presence_decl_fail.out; then
    echo "missing TOON has_at declaration type failure message" >&2
    cat /tmp/aether_toon_presence_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TOON_DEFAULTS_DECL_FAIL_FIXTURE" >/tmp/aether_toon_defaults_decl_fail.out 2>&1; then
    echo "expected TOON defaults declaration failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "call to 'toon_get_int_or' expects a Int third argument, but 'fallbackText' is Text" /tmp/aether_toon_defaults_decl_fail.out; then
    echo "missing TOON int default fallback type failure message" >&2
    cat /tmp/aether_toon_defaults_decl_fail.out >&2
    exit 1
fi
if ! grep -q "binding for 'wrongFlag' must use Bool when initialized from 'toon_get_bool_or'" /tmp/aether_toon_defaults_decl_fail.out; then
    echo "missing TOON bool default declaration type failure message" >&2
    cat /tmp/aether_toon_defaults_decl_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$COST_ZERO_FAIL_FIXTURE" >/tmp/aether_cost_zero_fail.out 2>&1; then
    echo "expected @cost zero-budget failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@cost budget must be greater than zero" /tmp/aether_cost_zero_fail.out; then
    echo "missing @cost zero-budget failure message" >&2
    cat /tmp/aether_cost_zero_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$COST_UNIT_FAIL_FIXTURE" >/tmp/aether_cost_unit_fail.out 2>&1; then
    echo "expected @cost unit failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "unsupported @cost unit 'ticks'" /tmp/aether_cost_unit_fail.out; then
    echo "missing @cost unit failure message" >&2
    cat /tmp/aether_cost_unit_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$COST_DETACHED_FAIL_FIXTURE" >/tmp/aether_cost_detached_fail.out 2>&1; then
    echo "expected detached @cost failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@cost must annotate the next function declaration" /tmp/aether_cost_detached_fail.out; then
    echo "missing detached @cost failure message" >&2
    cat /tmp/aether_cost_detached_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$COST_DUPLICATE_FAIL_FIXTURE" >/tmp/aether_cost_duplicate_fail.out 2>&1; then
    echo "expected duplicate @cost failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "duplicate @cost annotation before function declaration" /tmp/aether_cost_duplicate_fail.out; then
    echo "missing duplicate @cost failure message" >&2
    cat /tmp/aether_cost_duplicate_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_PRE_EMPTY_FAIL_FIXTURE" >/tmp/aether_contract_pre_empty_fail.out 2>&1; then
    echo "expected empty @pre failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@pre requires an expression" /tmp/aether_contract_pre_empty_fail.out; then
    echo "missing empty @pre failure message" >&2
    cat /tmp/aether_contract_pre_empty_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_POST_DETACHED_FAIL_FIXTURE" >/tmp/aether_contract_post_detached_fail.out 2>&1; then
    echo "expected detached @post failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@post must annotate the next function declaration" /tmp/aether_contract_post_detached_fail.out; then
    echo "missing detached @post failure message" >&2
    cat /tmp/aether_contract_post_detached_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_PURE_TRAILING_FAIL_FIXTURE" >/tmp/aether_contract_pure_trailing_fail.out 2>&1; then
    echo "expected trailing @pure syntax failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "@pure does not take arguments" /tmp/aether_contract_pure_trailing_fail.out; then
    echo "missing trailing @pure syntax failure message" >&2
    cat /tmp/aether_contract_pure_trailing_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_FAIL_PRE_FIXTURE" >/tmp/aether_contract_fail_pre.out 2>&1; then
    echo "expected precondition failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether @pre failed in inc" /tmp/aether_contract_fail_pre.out; then
    echo "missing precondition failure message" >&2
    cat /tmp/aether_contract_fail_pre.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$CONTRACT_FAIL_POST_FIXTURE" >/tmp/aether_contract_fail_post.out 2>&1; then
    echo "expected postcondition failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether @post failed in inc" /tmp/aether_contract_fail_post.out; then
    echo "missing postcondition failure message" >&2
    cat /tmp/aether_contract_fail_post.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$EFFECTS_FAIL_FIXTURE" >/tmp/aether_effects_fail.out 2>&1; then
    echo "expected effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'writeln' requires an fx block" /tmp/aether_effects_fail.out; then
    echo "missing effect-boundary failure message" >&2
    cat /tmp/aether_effects_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$TASK_ALIAS_FAIL_FIXTURE" >/tmp/aether_task_alias_fail.out 2>&1; then
    echo "expected task alias effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'task_spawn' requires an fx block" /tmp/aether_task_alias_fail.out; then
    echo "missing task alias effect-boundary failure message" >&2
    cat /tmp/aether_task_alias_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$AI_ALIAS_FAIL_FIXTURE" >/tmp/aether_ai_alias_fail.out 2>&1; then
    echo "expected ai alias effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'ai_chat' requires an fx block" /tmp/aether_ai_alias_fail.out; then
    echo "missing ai alias effect-boundary failure message" >&2
    cat /tmp/aether_ai_alias_fail.out >&2
    exit 1
fi

if env -u OPENAI_API_KEY "$AETHER_BIN" --no-cache "$RUNTIME_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_runtime_line_mapping_fail.out 2>&1; then
    echo "expected runtime line-mapping failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "OpenAIChatCompletions requires an API key" /tmp/aether_runtime_line_mapping_fail.out; then
    echo "missing runtime openai failure message" >&2
    cat /tmp/aether_runtime_line_mapping_fail.out >&2
    exit 1
fi
if ! grep -q "\\[Error Location\\] Offset: " /tmp/aether_runtime_line_mapping_fail.out; then
    echo "missing runtime error location" >&2
    cat /tmp/aether_runtime_line_mapping_fail.out >&2
    exit 1
fi
if ! grep -q "Line: 4" /tmp/aether_runtime_line_mapping_fail.out; then
    echo "missing mapped runtime line number" >&2
    cat /tmp/aether_runtime_line_mapping_fail.out >&2
    exit 1
fi
if env -u OPENAI_API_KEY "$AETHER_BIN" --diagnostics-json --no-cache "$RUNTIME_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_runtime_line_mapping_json.out 2>&1; then
    echo "expected runtime diagnostics-json failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '"phase":"runtime"' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json phase" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"kind":"runtime"' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json kind" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"file":"'"$RUNTIME_LINE_MAPPING_FAIL_FIXTURE"'"' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json file path" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"line":4' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json line number" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q 'OpenAIChatCompletions requires an API key' /tmp/aether_runtime_line_mapping_json.out; then
    echo "missing runtime diagnostics-json message" >&2
    cat /tmp/aether_runtime_line_mapping_json.out >&2
    exit 1
fi
if env -u OPENAI_API_KEY "$AETHER_BIN" --diagnostics-toon --no-cache "$RUNTIME_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_runtime_line_mapping_toon.out 2>&1; then
    echo "expected runtime diagnostics-toon failure but program succeeded" >&2
    exit 1
fi
if ! grep -q '^diagnostics\[1\]{severity,phase,kind,file,line,column,message,hint,raw}:$' /tmp/aether_runtime_line_mapping_toon.out; then
    echo "missing runtime diagnostics-toon header" >&2
    cat /tmp/aether_runtime_line_mapping_toon.out >&2
    exit 1
fi
if ! grep -q '"error","runtime","runtime","'"$RUNTIME_LINE_MAPPING_FAIL_FIXTURE"'",4,null,"OpenAIChatCompletions requires an API key via argument or OPENAI_API_KEY\."' /tmp/aether_runtime_line_mapping_toon.out; then
    echo "missing runtime diagnostics-toon row" >&2
    cat /tmp/aether_runtime_line_mapping_toon.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$PRINT_ALIAS_FAIL_FIXTURE" >/tmp/aether_print_alias_fail.out 2>&1; then
    echo "expected print alias effect-boundary failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether effect error: call to 'println' requires an fx block" /tmp/aether_print_alias_fail.out; then
    echo "missing print alias effect-boundary failure message" >&2
    cat /tmp/aether_print_alias_fail.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$INFERRED_LET_UNKNOWN_FAIL_FIXTURE" >/tmp/aether_inferred_let_unknown_fail.out 2>&1; then
    echo "expected inferred let rewrite failure but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether declaration rewrite error: cannot infer the type of 'answer' from its initializer" /tmp/aether_inferred_let_unknown_fail.out; then
    echo "missing inferred let rewrite failure message" >&2
    cat /tmp/aether_inferred_let_unknown_fail.out >&2
    exit 1
fi
if ! grep -q "hint: add an explicit type, for example \`let answer: Int = ...;\`." /tmp/aether_inferred_let_unknown_fail.out; then
    echo "missing inferred let rewrite failure hint" >&2
    cat /tmp/aether_inferred_let_unknown_fail.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$INFERRED_LET_UNKNOWN_FAIL_FIXTURE" >/tmp/aether_inferred_let_unknown_json.out 2>&1; then
    echo "expected inferred let rewrite failure with diagnostics-json but program succeeded" >&2
    exit 1
fi
if ! grep -q '"phase":"rewrite"' /tmp/aether_inferred_let_unknown_json.out; then
    echo "missing diagnostics-json rewrite phase" >&2
    cat /tmp/aether_inferred_let_unknown_json.out >&2
    exit 1
fi
if ! grep -q '"kind":"declaration"' /tmp/aether_inferred_let_unknown_json.out; then
    echo "missing diagnostics-json declaration kind" >&2
    cat /tmp/aether_inferred_let_unknown_json.out >&2
    exit 1
fi
if ! grep -q '"file":"'"$INFERRED_LET_UNKNOWN_FAIL_FIXTURE"'"' /tmp/aether_inferred_let_unknown_json.out; then
    echo "missing diagnostics-json file path" >&2
    cat /tmp/aether_inferred_let_unknown_json.out >&2
    exit 1
fi
if ! grep -q '"hint":"add an explicit type, for example `let answer: Int = ...;`."' /tmp/aether_inferred_let_unknown_json.out; then
    echo "missing diagnostics-json hint" >&2
    cat /tmp/aether_inferred_let_unknown_json.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-json --no-cache "$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_diagnostic_line_mapping_json.out 2>&1; then
    echo "expected diagnostic line-mapping failure with diagnostics-json but program succeeded" >&2
    exit 1
fi
if ! grep -q '"file":"'"$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE"'"' /tmp/aether_diagnostic_line_mapping_json.out; then
    echo "missing mapped diagnostics-json file path" >&2
    cat /tmp/aether_diagnostic_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"line":8' /tmp/aether_diagnostic_line_mapping_json.out; then
    echo "missing mapped diagnostics-json loop line" >&2
    cat /tmp/aether_diagnostic_line_mapping_json.out >&2
    exit 1
fi
if ! grep -q '"line":9' /tmp/aether_diagnostic_line_mapping_json.out; then
    echo "missing mapped diagnostics-json body line" >&2
    cat /tmp/aether_diagnostic_line_mapping_json.out >&2
    exit 1
fi
if "$AETHER_BIN" --diagnostics-toon --no-cache "$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE" >/tmp/aether_diagnostic_line_mapping_toon.out 2>&1; then
    echo "expected diagnostic line-mapping failure with diagnostics-toon but program succeeded" >&2
    exit 1
fi
if ! grep -q '^diagnostics\[3\]{severity,phase,kind,file,line,column,message,hint,raw}:$' /tmp/aether_diagnostic_line_mapping_toon.out; then
    echo "missing diagnostics-toon header" >&2
    cat /tmp/aether_diagnostic_line_mapping_toon.out >&2
    exit 1
fi
if ! grep -q '"scope","'"$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE"'".*,8,null,"identifier '\''i'\'' not in scope\."' /tmp/aether_diagnostic_line_mapping_toon.out; then
    echo "missing diagnostics-toon mapped loop line" >&2
    cat /tmp/aether_diagnostic_line_mapping_toon.out >&2
    exit 1
fi
if ! grep -q '"scope","'"$DIAGNOSTIC_LINE_MAPPING_FAIL_FIXTURE"'".*,9,null,"identifier '\''i'\'' not in scope\."' /tmp/aether_diagnostic_line_mapping_toon.out; then
    echo "missing diagnostics-toon mapped body line" >&2
    cat /tmp/aether_diagnostic_line_mapping_toon.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$PURE_FAIL_EFFECTFUL_FIXTURE" >/tmp/aether_pure_fail_effectful.out 2>&1; then
    echo "expected purity failure for effectful builtin but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether purity error: pure function 'noisy' cannot call effectful builtin 'writeln'" /tmp/aether_pure_fail_effectful.out; then
    echo "missing purity failure for effectful builtin" >&2
    cat /tmp/aether_pure_fail_effectful.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$PURE_FAIL_NON_PURE_CALL_FIXTURE" >/tmp/aether_pure_fail_non_pure_call.out 2>&1; then
    echo "expected purity failure for non-pure call but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether purity error: pure function 'wrapper' cannot call non-pure function 'noisy'" /tmp/aether_pure_fail_non_pure_call.out; then
    echo "missing purity failure for non-pure call" >&2
    cat /tmp/aether_pure_fail_non_pure_call.out >&2
    exit 1
fi

if "$AETHER_BIN" --no-cache "$PAR_FAIL_NON_CALL_FIXTURE" >/tmp/aether_par_fail_non_call.out 2>&1; then
    echo "expected par rewrite failure for non-call statement but program succeeded" >&2
    exit 1
fi
if ! grep -q "Aether par rewrite error: only direct call statements are allowed inside par blocks" /tmp/aether_par_fail_non_call.out; then
    echo "missing par rewrite failure message" >&2
    cat /tmp/aether_par_fail_non_call.out >&2
    exit 1
fi

"$AETHER_BIN" --no-cache "$TOON_COMMENT_ARITH_PASS_FIXTURE" >/tmp/aether_toon_comment_arith_pass.out
if ! grep -q '^Ada$' /tmp/aether_toon_comment_arith_pass.out; then
    echo "missing TOON comment arithmetic pass output" >&2
    cat /tmp/aether_toon_comment_arith_pass.out >&2
    exit 1
fi

"$AETHER_BIN" --no-cache "$TOON_NESTED_HELPERS_PASS_FIXTURE" >/tmp/aether_toon_nested_helpers_pass.out
if ! grep -q '^Ada 91$' /tmp/aether_toon_nested_helpers_pass.out; then
    echo "missing TOON nested helper pass output" >&2
    cat /tmp/aether_toon_nested_helpers_pass.out >&2
    exit 1
fi

echo "aether smoke tests passed"
