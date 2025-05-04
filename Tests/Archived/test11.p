program records;
type
  TRec = record
    x: integer;
    y: string;
  end;
var
  a: array[1..2] of TRec;
begin
  a[1].x := 10;
  a[1].y := 'Hello';
  a[2].x := a[1].x + 5;
  writeln(a[1].x, ' ', a[1].y);
  writeln(a[2].x);
end.
