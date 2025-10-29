program ClosureProcStore;

type
  TThunk = procedure;

var
  Stored: TThunk;
  Counter: integer;

procedure MakeStepper(start: integer);
var
  value: integer;
  procedure Step;
  begin
    value := value + 1;
    Counter := Counter + value;
  end;
begin
  value := start;
  Stored := @Step;
end;

var
  expected: integer;
begin
  Counter := 0;
  MakeStepper(5);
  Stored();
  Stored();
  expected := 6 + 7;
  if Counter <> expected then
    writeln('FAIL: stored procedure closure produced ', Counter, ' expected ', expected)
  else
  begin
    Counter := 100;
    MakeStepper(1);
    Stored();
    Stored();
    expected := 100 + 2 + 3;
    if Counter <> expected then
      writeln('FAIL: reassigned procedure closure produced ', Counter, ' expected ', expected)
    else
      writeln('PASS: capturing procedure closure stored and invoked');
  end;
end.
