# GoStyleClosureInterfaceDemo Walkthrough

The `Examples/pascal/base/GoStyleClosureInterfaceDemo` sample illustrates two advanced Pascal front-end features in PSCAL:

* **Go-style closures that capture state and escape their defining scope.**
* **Interface values that box a record pointer together with its virtual method table.**

This walkthrough dissects the program step by step so you can see how these features interact.

## Key types and declarations

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

* `TCounterFactory` is a function type alias. Instances of this type are closure values that return an `integer` on each invocation.
* `IRunnable` is a single-method interface used to expose a `Run` operation without exposing implementation details.
* `TClosureRunner` is a record with a virtual `Run` method. In the Pascal front end, records with virtual methods carry a hidden self pointer (`myself`) when invoked via an interface, similar to Go method sets. Its fields store a label and the closure that produces the next counter value.

## The closure factory

`MakeRunner` builds an `IRunnable` backed by a heap-allocated `TClosureRunner` record:

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

* `current` lives on `MakeRunner`'s stack frame, but the nested `Next` function forms a closure over it. When `MakeRunner` returns, the Pascal front end hoists `current` into heap storage so the closure can outlive the factory call.
* `Next` is assigned to `runner^.nextValue` as a `TCounterFactory`. Each invocation mutates the captured `current` and returns the updated count.
* `new(runner)` allocates `TClosureRunner` on the heap. Casting the pointer to `IRunnable` boxes both the pointer and the record’s vtable, mirroring Go’s interface boxing semantics.

## Virtual dispatch via the interface

`TClosureRunner.Run` uses the Pascal front end's implicit `myself` pointer (the interface receiver) to reach the boxed record:

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

* `myself` resolves to the `TClosureRunner` instance that lives behind the interface value.
* The stored `TCounterFactory` closure is invoked to advance the counter and fetch the next integer.
* The result is printed alongside the runner’s label, demonstrating that the record state (`labelText`) and the closure state (`current`) travel together through the interface indirection.

## Program flow

The main block wires everything together:

```pascal
fast := MakeRunner('fast', 0, 2);
slow := MakeRunner('slow', 10, -1);

for i := 1 to 3 do
    fast.Run;

slow.Run;
fast.Run;
slow.Run;
```

1. `fast` is a counter starting at `0` and incrementing by `2` each time `Run` is called.
2. `slow` starts at `10` and decrements by `1` each time.
3. The `for` loop drives `fast` three times, showing the closure retains its mutable state across calls.
4. Subsequent alternating calls demonstrate that each boxed runner maintains its own independent captured `current` value.

## Sample output

Running the program prints interleaved ticks that reflect the per-runner state:

```
fast tick=2
fast tick=4
fast tick=6
slow tick=9
fast tick=8
slow tick=8
```

The sequence confirms that:

* Each `Run` call dispatches through the `IRunnable` interface into the correct record instance.
* The closures created inside `MakeRunner` continue to mutate their captured `current` even after the factory returns.
* Distinct interface values (`fast` and `slow`) encapsulate separate heaps of state while sharing a common implementation.
