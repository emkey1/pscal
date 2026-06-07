# GoStyleClosureInterfaceDemo Walkthrough

The `Examples/pascal/base/GoStyleClosureInterfaceDemo` sample still shows the
core PSCAL idea set clearly:

* closure values can capture state and escape their defining scope
* interface values box a record receiver together with virtual dispatch metadata
* records, not classes, are the primary OO building block

Since that sample was first written, the Pascal front end has gained a few
extensions that sharpen the same design direction. This walkthrough explains
the original sample and then maps it onto the newer features.

## Core idea

The sample builds a concrete record that stores:

* ordinary record state such as a label
* a closure field that advances an internal counter
* a virtual method used through an interface

That is the basic PSCAL "Go-style OO" model:

* concrete behavior lives in records
* stateful callbacks are closures
* polymorphism happens through interfaces
* composition is preferred over class syntax

## Key declarations

```pascal
TCounterFactory = function: integer;

IRunnable = interface
  procedure Run;
end;

TClosureRunner = record
  labelText: string;
  nextValue: TCounterFactory;
  procedure Run; virtual;
end;
```

What each part means:

* `TCounterFactory` is a function-pointer type. Values of this type may be
  plain function pointers or capturing closure values.
* `IRunnable` exposes only behavior. Code using the interface does not need to
  know how the record stores its state.
* `TClosureRunner` is a concrete record with a virtual `Run` method and a
  stored closure field.

## Closure factory

The sample's factory returns an `IRunnable` backed by a heap-allocated record:

```pascal
function MakeRunner(const name: string; start, step: integer): IRunnable;
var
  current: integer;
  runner: ^TClosureRunner;

  function Next: integer;
  begin
    current := current + step;
    Next := current;
  end;
begin
  new(runner);
  current := start;
  runner^.labelText := name;
  runner^.nextValue := @Next;
  MakeRunner := IRunnable(runner);
end;
```

Important points:

* `Next` captures `current` and `step`.
* assigning `@Next` to `runner^.nextValue` allows that nested function to
  escape the factory scope
* the compiler preserves the captured state so later calls still mutate the
  same `current`
* casting `runner` to `IRunnable` boxes the receiver pointer and method table

This is still the canonical pattern when you want a named nested routine to
escape.

## Virtual dispatch and `myself`

The sample's virtual method uses the receiver pointer supplied by the runtime:

```pascal
procedure TClosureRunner.Run;
var
  value: integer;
  runner: ^TClosureRunner;
  next: TCounterFactory;
begin
  runner := myself;
  next := runner^.nextValue;
  value := next();
  writeln(runner^.labelText, ' tick=', value);
end;
```

In PSCAL record methods, the implicit receiver is `myself`. That is the current
front-end convention; `Self` is not the receiver keyword here.

The method demonstrates two independent state channels:

* record state, accessed through `runner^.labelText`
* closure state, accessed by calling `runner^.nextValue`

## What newer PSCAL supports now

The sample predates several useful extensions. They fit the same overall
philosophy rather than changing it.

### Embedded record composition

Records may now embed one base record with `inherit`:

```pascal
type
  TRunnerBase = record
    labelText: string;
    procedure Run; virtual;
  end;

  TCounterRunner = record
    inherit TRunnerBase;
    nextValue: TCounterFactory;
  end;
```

This is composition-oriented record embedding, not class inheritance. The base
record occupies the leading storage and its fields/methods are promoted onto
the derived record.

That means the closure/interface demo could now be factored into a reusable
base record plus a specialized embedded record without introducing `class` or
`object`.

### Anonymous routine literals inside routines

In addition to named nested closures like `@Next`, PSCAL now accepts anonymous
routine literals in expression position inside routines:

```pascal
nextValue := function: integer;
begin
  current := current + step;
  Result := current;
end;
```

or with parameters:

```pascal
transform := function(delta: integer): integer;
begin
  Result := base + delta;
end;
```

These literals are lowered to generated nested routines and use the same
closure-capture machinery as named nested routines.

For the sample's specific shape, both of these are now valid styles:

```pascal
runner^.nextValue := @Next;
```

and

```pascal
runner^.nextValue := function: integer;
begin
  current := current + step;
  Result := current;
end;
```

The named form remains a good choice when the logic is reused or when you want
clearer debugging symbols. The anonymous form is better when the callback is
truly local and single-use.

### Threading-related closure shorthand

Inside routines, `spawn(procedure begin ... end)` is also accepted as a
parameterless anonymous-procedure shorthand. That feature is adjacent to this
demo's closure model: it uses the same nested-routine lowering strategy, but
targets thread launch instead of proc-pointer storage.

When arguments must be passed to the thread entry, prefer:

```pascal
t := CreateThread(@Worker, argPtr);
```

rather than `spawn(...)`.

## Updated mental model

The current PSCAL model is best understood this way:

* use plain `record`s for concrete state and behavior
* use `virtual` methods when a record must satisfy an interface
* use `myself` inside record methods to access the active receiver
* use nested routines or anonymous routine literals when behavior must capture
  local state
* use `inherit BaseRecord;` when you want promoted embedded composition
* use interfaces when callers should depend on behavior rather than layout

That gives you something closer to Go's combination of:

* concrete structs
* method sets
* interface values
* closure-based callbacks

than to Delphi's `class` hierarchy model.

## Program flow in the original sample

The main block is still useful because it shows the behavioral payoff:

```pascal
fast := MakeRunner('fast', 0, 2);
slow := MakeRunner('slow', 10, -1);

for i := 1 to 3 do
  fast.Run;

slow.Run;
fast.Run;
slow.Run;
```

This confirms:

* each boxed interface value dispatches to the correct record instance
* each stored closure retains its own captured state
* record state and closure state travel together cleanly through interface use

## Sample output

```text
fast tick=2
fast tick=4
fast tick=6
slow tick=9
fast tick=8
slow tick=8
```

The output demonstrates that:

* interface dispatch resolves the correct `Run` implementation
* the `fast` and `slow` runners keep separate closure state
* the closure value remains valid after the factory returns

## Recommended style going forward

For new PSCAL code in this design family:

* prefer records plus interfaces over inventing class-like syntax
* prefer embedded records with `inherit` when you want promoted shared state or
  helper behavior
* prefer anonymous routine literals for short callback construction
* prefer named nested routines when the closure is conceptually reusable or
  deserves a real name
* keep `myself`-based receiver logic explicit in record methods

That keeps the language aligned with its current strengths and with the
long-term composition-first direction.
