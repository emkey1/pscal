program RoutineNestedFunction;
var
  globalBase: Integer;

function Factory(offset: Integer): Integer;
var
  localBase: Integer;

  function Accumulate(step: Integer): Integer;
  begin
    Accumulate := localBase + offset + step + globalBase;
  end;

begin
  localBase := offset * 2;
  Factory := Accumulate(1);
end;

begin
  globalBase := 7;
  writeln('result=', Factory(3));
  writeln('global=', globalBase);
end.