program RoutineNestedCapture;
var
  total: Integer;

procedure Accumulate(start: Integer);
var
  index: Integer;

  procedure Step(amount: Integer);
  begin
    total := total + amount;
  end;

begin
  for index := 1 to 3 do
  begin
    Step(index * start);
  end;
end;

begin
  total := 0;
  Accumulate(1);
  writeln('total=', total);
end.