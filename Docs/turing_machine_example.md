# Turing Machine Example

`Examples/pascal/base/TuringMachine` is an interactive single-tape Turing machine
simulator written in PSCAL Pascal. It started as a fixed 3-state busy-beaver
demo and has been expanded into a more general teaching/example program.

## Why Turing Machines Matter

Turing machines were introduced by Alan Turing in 1936 as an abstract model of
mechanical computation. The model is deliberately minimal:

- an infinite tape of cells
- a read/write head positioned on one cell
- a finite set of states
- a transition table that decides what to write, where to move, and which state to enter next

That tiny model turned out to be powerful enough to describe general algorithmic
computation. Turing’s work, along with related work by Church and others,
helped establish the modern notion of computability and the limits of what can
be decided by an algorithm.

In practice, nobody programs real computers as literal Turing machines, but the
model remains useful because it is:

- mathematically simple
- expressive enough to reason about computation in general
- a good way to demonstrate the difference between a machine description and the
  machine that executes it

## What The Example Does

The example provides:

- built-in sample machines
- step-by-step execution
- run-to-halt or run-to-limit execution
- tape visualization around the head
- transition-table display
- dynamic tape growth when the head moves beyond the current bounds
- loading transition tables from a plain text file

Built-in machines currently include:

- 2-state busy beaver
- 3-state busy beaver
- unary increment
- binary increment

## Running It

From the repository root:

```sh
build/bin/pascal Examples/pascal/base/TuringMachine
```

The main menu lets you choose a built-in machine or load one from disk.

## Interactive Commands

After a machine is loaded:

- `Enter` or `S`: execute one step
- `R`: run until halt or until the configured step limit is reached
- `T`: print the transition table
- `X`: reset the machine to its initial tape and start state
- `Q`: return to the main menu

The display shows:

- current machine name
- current state
- current step count
- head position
- blank symbol
- rule count
- a tape window centered around the head

The current head position is shown with brackets, for example:

```text
 .  .  1 [0] 1  .  .
```

## Machine File Format

The loader accepts a simple line-oriented format. Blank lines are ignored.
Lines beginning with `#` are comments. Inline comments beginning with `#` are
also ignored.

Supported directives:

- `name <text>`
- `blank <symbol>`
- `start <state>`
- `head <start|center|end>`
- `maxsteps <integer>`
- `tape <symbols>`
- `rule <state> <read> <write> <move> <next>`

State names:

- `HALT`
- `A` through `Z`

Tape symbols:

- digits such as `0` and `1`
- `.` for blank

Moves:

- `L` or `LEFT`
- `R` or `RIGHT`
- `S` or `STAY`

Example:

```text
# Binary incrementer
name Binary Increment From File
blank 0
start A
head end
maxsteps 24
tape 1011

rule A 1 0 L A
rule A 0 1 S HALT
```

You can also omit the `rule` keyword and write raw transition rows directly:

```text
A 1 0 L A
A 0 1 S HALT
```

## Notes On Semantics

- The simulator uses a single tape.
- The blank symbol is configurable, but the built-in visual display treats blank
  as `.`.
- The tape expands automatically when the head walks off either end.
- The example is deterministic: the first matching rule is taken, and rule order
  matters only if you accidentally provide duplicate `(state, read-symbol)`
  pairs.

## Why It Is Useful In This Repo

This example is a good stress test and demonstration for several Pascal-front
end features at once:

- records and nested record constructors
- enums
- arrays and dynamic resizing
- file I/O
- string parsing
- interactive console control flow

It is also a concrete example of PSCAL Pascal being used to build a small but
nontrivial interpreter inside the VM itself.
