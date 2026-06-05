program LocalRecordStoreBytecode;

type
  TItem = record
    X, Y: Integer;
    Value: Integer;
  end;

var
  Items: array[1..2] of TItem;

procedure SaveLike;
var
  It: TItem;
begin
  It := Items[1];
  WriteLn(It.X, ' ', It.Y, ' ', It.Value);
end;

begin
  Items[1].X := 10;
  Items[1].Y := 20;
  Items[1].Value := 30;
  SaveLike;
end.
