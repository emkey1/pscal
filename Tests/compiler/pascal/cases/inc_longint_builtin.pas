program IncLongIntBuiltin;

var
  generation: LongInt;
  step: SmallInt;

begin
  generation := 0;
  step := 3;

  Inc(generation);
  Inc(generation, step);
  Dec(generation, 1);

  if generation = 3 then
    Writeln('PASS: inc/dec longint')
  else
    Writeln('FAIL: generation=', generation);
end.
