program RoutineLocalShadow;
var
  counter: Integer;

procedure Run;
var
  counter: Integer;
begin
  counter := 10;
  writeln('inner=', counter);
end;

begin
  counter := 3;
  writeln('outer_before=', counter);
  Run;
  writeln('outer_after=', counter);
end.