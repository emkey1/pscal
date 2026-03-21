program VariantRecordOverlap;

type
  Pair = record
    case kind: Boolean of
      true: (left, right: Integer);
      false: (x, y: Integer);
  end;

var
  p: Pair;

begin
  p.kind := true;
  p.left := 11;
  p.right := 29;

  if (p.x = 11) and (p.y = 29) then
    Writeln('PASS: variant overlap')
  else
    Writeln('FAIL: variant overlap ', p.x, ' ', p.y);
end.
