program RoutineParameterLeak;

procedure Echo(value: Integer);
begin
  writeln('inside=', value);
end;

begin
  Echo(3);
  writeln(value);
end.