# Pscal Language Reference

Pscal implements a substantial subset of classic Pascal with a few
extensions for modern convenience.

## Basic Types
- `integer`
- `real`
- `boolean`
- `char`
- `string`
- enumerations
- sets
- records

## Control Flow
- `if ... then ... else`
- `case`
- `for`
- `while`
- `repeat ... until`
- `break`

## Subroutines
Functions and procedures support local variables and parameters.  Units
allow code to be organised into separate modules that export types,
variables and routines which are imported with the `uses` clause.

## Example
```pascal
program Demo;
var
  i: integer;
begin
  for i := 1 to 10 do
    writeln('Hello ', i);
end.
```

## Built‑in Procedures and Functions
See [Pscal_Builtins](Pscal_Builtins.md) for the complete list of
available routines.

