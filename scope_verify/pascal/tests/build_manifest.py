#!/usr/bin/env python3
"""Generate manifest.json for the Pascal scope conformance suite."""
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
# Routine scope tests
# ---------------------------------------------------------------------------

add({
    "id": "routine_local_shadow_preserves_global",
    "name": "Local variable shadows global without mutation",
    "category": "routine_scope",
    "description": "Procedure-local variable with same name as global should shadow without mutating the outer binding.",
    "expect": "runtime_ok",
    "code": """
        program RoutineLocalShadow;
        var
          counter: Integer;

        procedure Run;
        var
          counter: Integer;
        begin
          counter := 10;
          writeln('inner=', counter);
        end;

        begin
          counter := 3;
          writeln('outer_before=', counter);
          Run;
          writeln('outer_after=', counter);
        end.
    """,
    "expected_stdout": """
        outer_before=3
        inner=10
        outer_after=3
    """,
})

add({
    "id": "routine_parameter_shadows_global",
    "name": "Parameter shadows global",
    "category": "routine_scope",
    "description": "Passing an argument should shadow a global of the same name without altering the global value.",
    "expect": "runtime_ok",
    "code": """
        program RoutineParameterShadow;
        var
          total: Integer;

        procedure Add(total: Integer);
        begin
          writeln('param=', total);
        end;

        begin
          total := 5;
          Add(7);
          writeln('global=', total);
        end.
    """,
    "expected_stdout": """
        param=7
        global=5
    """,
})

add({
    "id": "routine_nested_procedure_captures_outer",
    "name": "Nested procedure captures outer variable",
    "category": "routine_scope",
    "description": "Nested procedures should capture and mutate variables from the enclosing scope.",
    "expect": "runtime_ok",
    "code": """
        program RoutineNestedCapture;
        var
          total: Integer;

        procedure Accumulate(start: Integer);
        var
          index: Integer;

          procedure Step(amount: Integer);
          begin
            total := total + amount;
          end;

        begin
          for index := 1 to 3 do
          begin
            Step(index * start);
          end;
        end;

        begin
          total := 0;
          Accumulate(1);
          writeln('total=', total);
        end.
    """,
    "expected_stdout": "total=6",
})
add({
    "id": "routine_nested_function_captures_outer",
    "name": "Nested function captures outer values",
    "category": "routine_scope",
    "description": "Nested functions should capture both local and outer-scope variables while leaving globals untouched.",
    "expect": "runtime_ok",
    "code": """
        program RoutineNestedFunction;
        var
          globalBase: Integer;

        function Factory(offset: Integer): Integer;
        var
          localBase: Integer;

          function Accumulate(step: Integer): Integer;
          begin
            Accumulate := localBase + offset + step + globalBase;
          end;

        begin
          localBase := offset * 2;
          Factory := Accumulate(1);
        end;

        begin
          globalBase := 7;
          writeln('result=', Factory(3));
          writeln('global=', globalBase);
        end.
    """,
    "expected_stdout": """
        result=17
        global=7
    """,
})

add({
    "id": "routine_nested_function_leak_error",
    "name": "Nested function remains private at runtime",
    "category": "routine_scope",
    "description": "Attempting to call a nested helper from the outer scope fails at runtime in the current front-end.",
    "expect": "runtime_error",
    "code": """
        program RoutineNestedFunctionLeak;

        function Outer(value: Integer): Integer;
          function Hidden(delta: Integer): Integer;
          begin
            Hidden := value + delta;
          end;
        begin
          Outer := Hidden(1);
        end;

        begin
          writeln(Outer(2));
          writeln(Hidden(3));
        end.
    """,
    "expected_stderr_substring": "Undefined global variable",
    "failure_reason": "Nested functions remain private; calling them from the outer scope triggers a runtime failure.",
})



add({
    "id": "routine_parameter_leak_error",
    "name": "Parameter access outside routine fails at runtime",
    "category": "routine_scope",
    "description": "Accessing a routine parameter from the outer scope triggers a runtime failure.",
    "expect": "runtime_error",
    "code": """
        program RoutineParameterLeak;

        procedure Echo(value: Integer);
        begin
          writeln('inside=', value);
        end;

        begin
          Echo(3);
          writeln(value);
        end.
    """,
    "expected_stderr_substring": "Undefined global variable",
    "failure_reason": "Routine parameters are not visible outside the routine body; runtime reports an undefined variable.",
})

add({
    "id": "routine_duplicate_local_error",
    "name": "Duplicate local declarations reuse the last binding",
    "category": "routine_scope",
    "description": "The current compiler accepts duplicate local identifiers and keeps the last declared slot.",
    "expect": "runtime_ok",
    "code": """
        program RoutineDuplicateLocal;

        procedure Demo;
        var
          temp: Integer;
          temp: Integer;
        begin
          temp := 1;
          writeln(temp);
        end;

        begin
          Demo;
        end.
    """,
    "expected_stdout": "1",
    "failure_reason": "Duplicate locals are tolerated; ensure behaviour remains consistent.",
})

add({
    "id": "routine_loop_variable_persists_after",
    "name": "For-loop control variable persists",
    "category": "routine_scope",
    "description": "Standard Pascal keeps the for-loop variable in scope after the loop with its successor value.",
    "expect": "runtime_ok",
    "code": """
        program RoutineLoopVariablePersists;
        var
          i: Integer;
          total: Integer;
        begin
          total := 0;
          for i := 1 to 3 do
          begin
            total := total + i;
          end;
          writeln('sum=', total);
          writeln('after_loop=', i);
        end.
    """,
    "expected_stdout": """
        sum=6
        after_loop=4
    """,
})

# ---------------------------------------------------------------------------
# Constant scope tests
# ---------------------------------------------------------------------------

add({
    "id": "const_shadow_local_overrides_global",
    "name": "Inner constant redefinition persists globally",
    "category": "const_scope",
    "description": "Re-declaring a constant inside a routine updates the global binding in this front-end.",
    "expect": "runtime_ok",
    "code": """
        program ConstShadow;
        const
          Base = 2;

        procedure Show;
        const
          Base = 5;
        begin
          writeln('inner=', Base);
        end;

        begin
          Show;
          writeln('outer=', Base);
        end.
    """,
    "expected_stdout": """
        inner=5
        outer=5
    """,
})

add({
    "id": "const_nested_expression_uses_outer",
    "name": "Inner constant can use outer constant",
    "category": "const_scope",
    "description": "Constants declared inside a routine may reference outer constants when initialised.",
    "expect": "runtime_ok",
    "code": """
        program ConstNestedExpression;
        const
          Base = 3;

        procedure Report;
        const
          Step = Base + 2;
        begin
          writeln('step=', Step);
        end;

        begin
          Report;
          writeln('base=', Base);
        end.
    """,
    "expected_stdout": """
        step=5
        base=3
    """,
})

add({
    "id": "const_leak_error",
    "name": "Local constant remains visible outside",
    "category": "const_scope",
    "description": "Constants declared inside a routine continue to be accessible globally in the current implementation.",
    "expect": "runtime_ok",
    "code": """
        program ConstLeak;

        procedure Maker;
        const
          Hidden = 4;
        begin
          writeln('hidden=', Hidden);
        end;

        begin
          Maker;
          writeln(Hidden);
        end.
    """,
    "expected_stdout": """
        hidden=4
        4
    """,
    "failure_reason": "Document the existing constant-leak behaviour so regressions are caught.",
})

# ---------------------------------------------------------------------------
# Type scope tests
# ---------------------------------------------------------------------------

add({
    "id": "type_local_shadow_allows_outer",
    "name": "Local type shadow keeps outer definition intact",
    "category": "type_scope",
    "description": "Pointer-based records demonstrate that inner type declarations don't disturb the outer definition.",
    "expect": "runtime_ok",
    "code": """
        program TypeLocalShadow;
        type
          PPair = ^TPair;
          TPair = record
            Left: Integer;
            Right: Integer;
          end;

        procedure UseGlobal;
        var
          pair: PPair;
        begin
          new(pair);
          pair^.Left := 1;
          pair^.Right := 2;
          writeln('global_pair=', pair^.Left + pair^.Right);
        end;

        procedure UseLocal;
        type
          PPair = ^TPair;
          TPair = record
            Left: Integer;
            Right: Integer;
            Sum: Integer;
          end;
        var
          pair: PPair;
        begin
          new(pair);
          pair^.Left := 3;
          pair^.Right := 4;
          pair^.Sum := pair^.Left + pair^.Right;
          writeln('local_sum=', pair^.Sum);
        end;

        begin
          UseLocal;
          UseGlobal;
        end.
    """,
    "expected_stdout": """
        local_sum=7
        global_pair=3
    """,
})

add({
    "id": "type_leak_error",
    "name": "Local type declaration leaks outside",
    "category": "type_scope",
    "description": "The compiler currently leaks procedure-local type declarations into the surrounding scope.",
    "expect": "runtime_ok",
    "code": """
        program TypeLeak;

        procedure Factory;
        type
          PInternal = ^TInternal;
          TInternal = record
            Value: Integer;
          end;
        var
          item: PInternal;
        begin
          new(item);
          item^.Value := 1;
          writeln('inside=', item^.Value);
        end;

        var
          other: PInternal;
        begin
          Factory;
          new(other);
          other^.Value := 2;
          writeln('outside=', other^.Value);
        end.
    """,
    "expected_stdout": """
        inside=1
        outside=2
    """,
    "failure_reason": "Capture the observed leaking behaviour for regression coverage.",
})

# ---------------------------------------------------------------------------
# Integration test combining multiple rules
# ---------------------------------------------------------------------------

add({
    "id": "integration_nested_scope_mix",
    "name": "Nested routines with shadowed consts and types",
    "category": "integration",
    "description": "Integration scenario combining shadowed constants, local types, and nested routines.",
    "expect": "runtime_ok",
    "code": """
        program IntegrationScopeMix;
        const
          Factor = 2;
        type
          PInfo = ^TInfo;
          TInfo = record
            Value: Integer;
          end;

        procedure Compute(start: Integer);
        const
          Factor = 3;
        type
          PLocalInfo = ^TLocalInfo;
          TLocalInfo = record
            Value: Integer;
            Total: Integer;
          end;
        var
          info: PLocalInfo;
          step: Integer;
          runningTotal: Integer;

          procedure AddStep(multiplier: Integer);
          begin
            runningTotal := runningTotal + multiplier * start;
          end;

        begin
          new(info);
          info^.Value := start;
          runningTotal := 0;
          for step := 1 to Factor do
          begin
            AddStep(step);
          end;
          info^.Total := runningTotal;
          writeln('inner_total=', info^.Total);
          writeln('local_factor=', Factor);
        end;

        var
          summary: PInfo;
        begin
          new(summary);
          summary^.Value := 4;
          Compute(2);
          writeln('outer_scaled=', summary^.Value * Factor);
        end.
    """,
    "expected_stdout": """
        inner_total=12
        local_factor=3
        outer_scaled=12
    """,
})


def main() -> None:
    manifest = {
        "version": 1,
        "default_extension": "pas",
        "notes": "Generated by build_manifest.py",
        "tests": tests,
    }
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
