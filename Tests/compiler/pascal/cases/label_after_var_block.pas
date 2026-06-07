program LabelAfterVarBlock;

procedure RunTest;
var
  value: Integer;
label
  Done;
begin
  value := 1;
  goto Done;
  value := 2;
Done:
  writeln('PASS: label after var block ', value);
end;

begin
  RunTest;
end.
