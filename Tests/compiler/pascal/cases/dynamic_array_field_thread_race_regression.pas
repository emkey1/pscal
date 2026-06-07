program TestDynamicArrayFieldThreadRaceRegression;

type
  TGrid = array of array of Integer;

  TShared = record
    Matrix: TGrid;
    procedure Writer; virtual;
  end;

var
  SharedState: ^TShared;

procedure TShared.Writer;
var
  myselfRef: ^TShared;
  nextA, nextB: TGrid;
  i: Integer;
begin
  myselfRef := myself;
  SetLength(nextA, 6, 6);
  SetLength(nextB, 6, 6);
  nextA[2][2] := 1;
  nextB[2][2] := 2;

  for i := 1 to 100000 do
  begin
    if Odd(i) then
      myselfRef^.Matrix := nextA
    else
      myselfRef^.Matrix := nextB;
  end;
end;

procedure WriterThread(selfPtr: ^TShared);
var
  sharedRef: ^TShared;
begin
  sharedRef := selfPtr;
  sharedRef^.Writer();
end;

var
  threadId: Integer;
  snap: TGrid;
  i, cell: Integer;
begin
  new(SharedState);
  SetLength(SharedState^.Matrix, 6, 6);

  threadId := CreateThread(@WriterThread, SharedState);
  for i := 1 to 100000 do
  begin
    snap := SharedState^.Matrix;
    if Length(snap) <> 6 then writeln('FAIL: concurrent outer length');
    if Length(snap[0]) <> 6 then writeln('FAIL: concurrent inner length');
    cell := snap[2][2];
    if (cell <> 0) and (cell <> 1) and (cell <> 2) then
      writeln('FAIL: concurrent cell contents');
  end;

  join threadId;
  dispose(SharedState);
  writeln('PASS: dynamic array field thread race regression');
end.
