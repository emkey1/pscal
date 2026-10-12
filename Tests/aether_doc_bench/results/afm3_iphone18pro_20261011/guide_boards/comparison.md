| variant | cases | FA | FA 95% | FX | silent-wrong | coded | uncoded | not sent | prompt tokens (median) |
|---|---|---|---|---|---|---|---|---|---|
| card | 105 | 10 (10%) | 5-17% | 11 | 22 | 73 | 0 | 0 | 2017 |
| v6 | 105 | 4 (4%) | 1-9% | 7 | 23 | 75 | 3 | 0 | 2301 |
| g3500 | 105 | 28 (27%) | 19-36% | 31 | 19 | 50 | 8 | 0 | 3451 |
| g4000 | 105 | 19 (18%) | 12-27% | 26 | 36 | 45 | 5 | 0 | 3948 |
| g3700 | 105 | 21 (20%) | 13-29% | 33 | 30 | 48 | 6 | 0 | 3713 |

| task | card | v6 | g3500 | g4000 | g3700 |
|---|---|---|---|---|---|
| hello_fx | www | www | PPP | PPP | PPP |
| hello_const_name | PwP | www | www | www | www |
| classify_scores | eee | eee | www | eee | eee |
| range_loop_squares | eee | eee | eee | www | www |
| loop_until_threshold | www | eee | uuu | uuu | uuu |
| nested_multiply_grid | www | eee | www | www | www |
| inline_if_average | eee | eee | eee | eee | eee |
| real_format_circle | eee | wew | www | www | www |
| percentage_format | www | PPP | www | www | www |
| eligibility_bool_logic | eee | eee | PwP | PeP | PPP |
| string_checks | eee | eee | eee | eee | eee |
| type_init_point | eee | eee | eee | eee | eee |
| inventory_status | eee | eee | eee | eee | eee |
| two_type_methods | eee | eee | eee | eee | eee |
| tuple_destructure | eee | www | eee | www | eee |
| tuple_order_post | www | eee | PPP | eee | eee |
| dynamic_array_rollup | eww | eee | eee | eee | eee |
| array_minmax | www | eee | eee | eee | PPP |
| contract_normalize | eee | eee | ueu | wew | PPP |
| module_function_import | eee | eee | PPP | PPP | PPP |
| module_const_import | eee | www | PPP | www | www |
| toon_inline_extract | eee | eee | uuu | PPP | www |
| toon_parse_file_simple | eee | eee | eee | www | www |
| toon_type_probe | eee | eee | eee | www | eee |
| toon_safe_nested_codes | eee | eee | eee | eee | eee |
| toon_deep_nested | eee | eee | eee | eee | eee |
| real_mean_format | PPP | uuu | PPP | PPP | PPP |
| clamp_scores | eee | www | PeP | ePu | eee |
| count_by_category | eee | eee | eee | uPe | uuu |
| recursion_factorial | PwP | www | PPP | www | eee |
| mstream_roundtrip | eee | eee | www | www | www |
| tuple_recursive_digits | eee | eee | PPP | ewe | eee |
| field_defaults_gauge | PPP | ePe | eee | PPP | eee |
| digit_separators_sum | eee | www | eee | eee | www |
| module_pure_export | eee | eee | PPP | eee | PPP |

P pass, w silent-wrong, e coded error, u uncoded error, - not sent; one letter per repeat.

first errors, card:
   23  [SYN-001] Aether parser error: 'return' is not Aether syntax.
    9  [SYN-001] Aether parser error: 'import' is not Aether syntax.
    8  [SYN-001] Aether parser error: unexpected token in block; expected a statement.
    4  [FX-001] Aether effect error: call to 'print' requires an fx block.
    3  [FX-001] Aether effect error: call to 'println' requires an fx block.
    3  [TYPE-001] Aether declaration parser error: 'ret' requires a type or an initializer.
    3  [SYN-001] Aether parser error: const declaration requires '= value'.
    3  [SCOPE-001] identifier 'MemoryStream' not in scope.

first errors, v6:
   24  [SYN-001] Aether parser error: 'return' is not Aether syntax.
    6  [SYN-001] Aether parser error: const declaration requires '= value'.
    6  [SYN-001] Aether parser error: 'import' is not Aether syntax.
    5  [FX-001] Aether effect error: call to 'print' requires an fx block.
    5  [SYN-001] Aether parser error: unexpected token in block; expected a statement.
    4  [SYN-001] Aether parser error: expected ')' to close parenthesized expression (opened at line 8
    3  [SYN-001] Aether parser error: expected ')' to close parenthesized expression (opened at line 6
    3  [TYPE-001] Aether declaration parser error: cannot infer the type of 'result' from its initiali

first errors, g3500:
   13  [SYN-001] Aether parser error: unexpected token in block; expected a statement.
    6  [SYN-001] Aether parser error: 'return' is not Aether syntax.
    4  [SYN-001] Aether syntax error: unexpected token '='; this construct could not be parsed.
    3  L5: cannot assign to constant 'LEVEL'.
    3  [SYN-001] Aether parser error: expected '}' to close if-expression branch.
    3  [SCOPE-001] Aether scope parser error: identifier 'Point' not in scope.
    3  [SCOPE-001] identifier 'x' not in scope.
    3  [FX-001] Aether effect error: call to 'println' requires an fx block.

first errors, g4000:
    9  [SYN-001] Aether parser error: const declaration requires '= value'.
    7  [SYN-001] Aether parser error: unexpected token in block; expected a statement.
    6  [SCOPE-001] identifier 'x' not in scope.
    5  [SYN-001] Aether parser error: 'return' is not Aether syntax.
    3  L5: cannot assign to constant 'LEVEL'.
    3  [TYPE-001] Aether declaration parser error: 'hi' requires a type or an initializer.
    3  [TYPE-001] Aether declaration parser error: cannot infer the type of the collection in `loop i 
    3  [SCOPE-001] identifier 'mean' not in scope.

first errors, g3700:
    9  [SYN-001] Aether parser error: 'return' is not Aether syntax.
    4  [SYN-001] Aether parser error: unexpected token in block; expected a statement.
    3  [SYN-001] Aether parser error: const declaration requires '= value'.
    3  L6: cannot assign to constant 'LEVEL'.
    3  [SYN-001] Aether parser error: expected ')' to close parenthesized expression (opened at line 7
    3  [SCOPE-001] identifier 'x' not in scope.
    3  [SYN-001] Aether type parser error: type fields must end with ';', not ','.
    3  [TYPE-001] Aether declaration parser error: 'hi' requires a type or an initializer.
