# Closures (and Friends) for PSCAL Pascal Developers

This is the practical guide to PSCAL Pascal closures, function/procedure
pointers, interface boxing, and the newer composition-oriented record features.
If you want to know what actually works today, start here.

## Short version

PSCAL Pascal now supports all of these patterns inside routines:

* returning a nested routine as a closure
* storing closures in globals, records, and interface-backed objects
* using anonymous `function ... begin ... end` and `procedure ... begin ... end`
  literals in expression context
* spawning parameterless anonymous procedures with `spawn(procedure ... end)`
* composing records with `inherit BaseRecord;`

The mental model is:

* a nested routine that captures outer locals becomes a heap-backed closure
* a record with `virtual` methods can be boxed behind an interface
* `myself` is the implicit receiver inside record methods
* embedded records are promoted composition, not `class` inheritance

## The two closure styles

PSCAL supports both named nested closures and anonymous routine literals.

### Named nested closure

```pascal
type
  TStep = function(): Integer;

function MakeCounter(start: Integer): TStep;
var
  value: Integer;

  function Next: Integer;
  begin
    value := value + 1;
    Next := value;
  end;
begin
  value := start;
  MakeCounter := @Next;
end;
```

Use this when the callback deserves a real name or when the body is large
enough that a named helper reads better.

### Anonymous routine literal

```pascal
type
  TStep = function(delta: Integer): Integer;

function MakeAdder(base: Integer): TStep;
begin
  Result := function(delta: Integer): Integer;
  begin
    Result := base + delta;
  end;
end;
```

Use this when the callback is single-use and tightly local to the surrounding
routine.

Anonymous routine literals are currently supported only inside routines, not at
top level.

## What counts as a closure?

A routine is a closure when it captures something from an outer routine, for
example:

* a local variable
* a parameter
* a `var` parameter

Example:

```pascal
function MakeAccumulator(start: Integer): function(x: Integer): Integer;
var
  total: Integer;
begin
  total := start;
  Result := function(x: Integer): Integer;
  begin
    total := total + x;
    Result := total;
  end;
end;
```

`total` outlives `MakeAccumulator`, so PSCAL moves the captured state into a
heap-managed closure environment.

## What actually happens under the hood

You do not need to memorize compiler internals, but the behavior is easier to
trust if you know the high-level flow:

1. semantic analysis records which nested routines capture outer locals
2. escaping routines are marked so code generation emits closure construction
3. the VM allocates a closure environment and retains it while references exist
4. by-reference captures stay shared; plain value captures are boxed

Practical result:

* you can safely return a capturing routine
* you can safely store it in a record field or global
* multiple closures can share the same captured state when appropriate

## The receiver rule: `myself`

Inside PSCAL record methods, the implicit receiver is `myself`, not `Self`.

Example:

```pascal
type
  PRunner = ^TRunner;
  TRunner = record
    labelText: String;
    procedure Print; virtual;
  end;

procedure TRunner.Print;
var
  runner: PRunner;
begin
  runner := myself;
  Writeln(runner^.labelText);
end;
```

This matters for closure-heavy OO code because interface calls seed `myself`
before dispatch.

## Closures inside records

This is the core PSCAL pattern for Go-style stateful behavior:

```pascal
type
  TCounterFactory = function(): Integer;

  TClosureRunner = record
    labelText: String;
    nextValue: TCounterFactory;
    procedure Run; virtual;
  end;

procedure TClosureRunner.Run;
var
  runner: ^TClosureRunner;
begin
  runner := myself;
  Writeln(runner^.labelText, ' tick=', runner^.nextValue());
end;
```

You can populate `nextValue` with either:

* `@NamedNestedRoutine`
* an anonymous function literal

Both are valid closure values.

## Interfaces and closures fit together

A common PSCAL design is:

* build a concrete record
* store closures in its fields
* expose only an interface

Example shape:

```pascal
type
  IRunnable = interface
    procedure Run;
  end;

  TRunner = record
    labelText: String;
    nextValue: function(): Integer;
    procedure Run; virtual;
  end;
```

Then:

```pascal
runner^.nextValue := function: Integer;
begin
  current := current + 1;
  Result := current;
end;

iface := IRunnable(runner);
```

The interface boxes:

* the receiver pointer
* the record’s dispatch metadata

The closure field keeps its own captured environment. So record state and
closure state move together cleanly.

## Embedded-record composition

PSCAL now supports:

```pascal
type
  TRunnerBase = record
    labelText: String;
    procedure Run; virtual;
  end;

  TCounterRunner = record
    inherit TRunnerBase;
    nextValue: function(): Integer;
  end;
```

This is the preferred reuse story for PSCAL OO code:

* plain records
* one embedded base record
* promoted base fields and methods
* interfaces for abstraction

This is not `class` inheritance. Think Go embedding, not Delphi `class`.

## Thread-related closure support

There are two separate threading stories:

### `spawn(procedure begin ... end)`

Valid inside routines for parameterless anonymous procedures:

```pascal
tid := spawn(procedure
             begin
               counter := counter + 1;
             end);
```

This is convenient for short fire-and-forget work that captures outer locals.

### `CreateThread(@Proc, arg)`

Use this when the worker needs an explicit pointer argument:

```pascal
t := CreateThread(@Worker, payloadPtr);
WaitForThread(t);
```

Prefer this form for classic worker-entry APIs and explicit payload passing.

## Patterns that work well

### Returning a closure

```pascal
counter := MakeCounter(10);
Writeln(counter());
Writeln(counter());
```

### Storing a closure in a global

```pascal
var
  Stored: function(): Integer;

Stored := MakeCounter(2);
Writeln(Stored());
```

### Passing an anonymous function as a callback

```pascal
UseAdder('inline=', function(delta: Integer): Integer;
                    begin
                      Result := delta * 2;
                    end);
```

### Boxing a closure-backed record behind an interface

```pascal
runner^.nextValue := @Next;
iface := IRunnable(runner);
iface.Run;
```

## Common mistakes

### Using `Self` instead of `myself`

Wrong:

```pascal
selfPtr := @Self;
```

Right:

```pascal
selfPtr := myself;
```

### Forgetting that top-level anonymous routine literals are not supported

This works only inside routines:

```pascal
Result := function: Integer;
begin
  Result := 42;
end;
```

### Mixing type syntax and value syntax

Wrong:

```pascal
destroy(sensor as TBaseSensor.fMutex);
```

Right:

```pascal
destroy((sensor as TBaseSensor).fMutex);
```

### Treating embedded-record composition like `class` inheritance

`inherit BaseRecord;` promotes a base record’s fields and methods. It does not
turn PSCAL records into Delphi classes.

## Limits to remember

Current limitations worth knowing:

* anonymous routine literals are routine-local only
* `spawn(procedure ... end)` is procedure-only and parameterless
* embedded records currently allow one base record
* global-scope capturing closures are still rejected

## Recommended style

For new PSCAL Pascal code:

* use named nested routines when the callback has conceptual weight
* use anonymous routine literals when the callback is small and local
* use `myself` explicitly in record methods
* use `inherit BaseRecord;` for shared record behavior/state
* use interfaces when callers should depend on behavior instead of layout

## Where to read next

* [go_style_closure_interface_demo.md](go_style_closure_interface_demo.md)
  for the composition/interface walkthrough
* [pascal_language_reference.md](pascal_language_reference.md) for the complete
  Pascal syntax reference
* [pascal_overview.md](pascal_overview.md) for the broad language/runtime tour
