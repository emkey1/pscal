program VariantRecordUntagged;

type
  Cell = record
    kind: Integer;
    case Byte of
      0: (name: String);
      1: (value: Integer);
  end;

var
  c: Cell;

begin
  c.kind := 1;
  c.value := 42;

  if (c.kind = 1) and (c.value = 42) then
    Writeln('PASS: untagged variant record')
  else
    Writeln('FAIL: untagged variant record');
end.
