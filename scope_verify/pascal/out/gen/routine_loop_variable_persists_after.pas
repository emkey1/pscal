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