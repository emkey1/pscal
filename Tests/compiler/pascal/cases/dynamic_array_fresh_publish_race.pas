program DynamicArrayFreshPublishRace;

type
  TGrid = array of array of Integer;

  TEngine = record
    Matrix: TGrid;
    procedure Writer; virtual;
  end;

var
  Engine: ^TEngine;

function BuildGrid(v: Integer): TGrid;
var
  nextGrid: TGrid;
begin
  SetLength(nextGrid, 6, 6);
  nextGrid[2][2] := v;
  BuildGrid := nextGrid;
end;

procedure TEngine.Writer;
var
  me: ^TEngine;
  i: Integer;
begin
  me := myself;
  for i := 0 to 3999 do
  begin
    me^.Matrix := BuildGrid(i mod 3);
  end;
end;

procedure WriterThread(selfPtr: ^TEngine);
var
  me: ^TEngine;
begin
  me := selfPtr;
  me^.Writer();
end;

procedure RunTest;
var
  threadId: Integer;
  i, v: Integer;
begin
  new(Engine);
  SetLength(Engine^.Matrix, 6, 6);
  threadId := CreateThread(@WriterThread, Engine);

  for i := 1 to 2000 do
  begin
    if Length(Engine^.Matrix) <> 6 then
      writeln('FAIL: outer length mismatch');
    if Length(Engine^.Matrix[0]) <> 6 then
      writeln('FAIL: inner length mismatch');
    v := Engine^.Matrix[2][2];
    if (v < 0) or (v > 2) then
      writeln('FAIL: invalid published value');
  end;

  join threadId;
  dispose(Engine);
  writeln('PASS: dynamic array fresh publish race');
end;

begin
  RunTest;
end.
