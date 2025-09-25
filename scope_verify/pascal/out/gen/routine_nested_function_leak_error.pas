program RoutineNestedFunctionLeak;

function Outer(value: Integer): Integer;
  function Hidden(delta: Integer): Integer;
  begin
    Hidden := value + delta;
  end;
begin
  Outer := Hidden(1);
end;

begin
  writeln(Outer(2));
  writeln(Hidden(3));
end.