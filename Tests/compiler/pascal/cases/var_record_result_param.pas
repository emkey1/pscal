program VarRecordResultParam;

type
  MoveType = record
    fromRow, fromCol, toRow, toCol: Integer;
    capturedRow, capturedCol: Integer;
  end;

procedure ClearMove(var m: MoveType);
begin
  m.fromRow := 0;
  m.fromCol := 0;
  m.toRow := 0;
  m.toCol := 0;
  m.capturedRow := 0;
  m.capturedCol := 0;
end;

function MakeMove(flag: Boolean): MoveType;
begin
  if flag then
    ClearMove(MakeMove)
  else
    ClearMove(MakeMove);
end;

begin
  MakeMove(true);
  Writeln('PASS: var record result param');
end.
