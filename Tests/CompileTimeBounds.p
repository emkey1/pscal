program CompileTimeBounds;

var
  arrMul: array[1..2*3] of integer;
  arrMinus: array[1..10-4] of integer;
  arrDiv: array[1..8/2] of integer;
  arrMod: array[1..20 mod 7] of integer;
  arrNested: array[0..(640 * 480 * 4) - 1] of integer;

begin
  arrMul[1] := 42;
  writeln((640 * 480 * 4) - 1);
end.
