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