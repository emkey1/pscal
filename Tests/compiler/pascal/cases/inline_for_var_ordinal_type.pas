program InlineForVarOrdinalType;

var
  total: Integer;

procedure RunIt;
begin
  total := 0;
  for var i := 1 to 5 do
    total := total + i;
end;

begin
  RunIt;
  Writeln('PASS: ', total);
end.
