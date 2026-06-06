program ForRecordFieldControlVar;

type
  TCursor = record
    R, C: Integer;
  end;

var
  cur: TCursor;
  Map: array[1..2, 1..3] of Integer;
  total: Integer;

begin
  total := 0;

  for cur.R := 1 to 2 do
    for cur.C := 1 to 3 do
      Map[cur.R, cur.C] := cur.R * 10 + cur.C;

  for cur.R := 1 to 2 do
    for cur.C := 1 to 3 do
      total := total + Map[cur.R, cur.C];

  Writeln('PASS: ', total);
end.
