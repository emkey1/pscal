program AnonymousRecordGlobalFields;

var
  point: record
    row, col: Integer;
  end;

begin
  point.row := 3;
  point.col := 4;

  if (point.row = 3) and (point.col = 4) then
    Writeln('PASS: anonymous record global fields')
  else
    Writeln('FAIL: anonymous record global fields');
end.
