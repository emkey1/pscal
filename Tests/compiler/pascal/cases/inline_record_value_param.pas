program InlineRecordValueParam;

type
  SearchMoveType = record
    isJump: Boolean;
    stepCount: Integer;
    captureCount: Integer;
    pathRows: array[1..8] of Integer;
    pathCols: array[1..8] of Integer;
  end;

function HasSearchMove(m: SearchMoveType): Boolean; inline;
begin
  HasSearchMove := m.stepCount >= 2;
end;

function Bump(n: Integer): Integer; inline;
begin
  Bump := n + 1;
end;

function GetBestMove(seed: Integer): SearchMoveType;
begin
  GetBestMove.isJump := false;
  GetBestMove.stepCount := 2;
  GetBestMove.captureCount := 0;
  GetBestMove.pathRows[1] := seed;
  GetBestMove.pathCols[1] := seed + 1;
end;

procedure Probe;
var
  aiMove: SearchMoveType;
  junk0, junk1, junk2, junk3, junk4, junk5, junk6, junk7: Integer;
begin
  junk0 := 0;
  junk1 := 1;
  junk2 := 2;
  junk3 := 3;
  junk4 := 4;
  junk5 := 5;
  junk6 := 6;
  junk7 := 7;
  junk0 := Bump(junk0);
  junk1 := Bump(junk1);

  aiMove := GetBestMove(junk0 + junk1 + junk2 + junk3 + junk4 + junk5 + junk6 + junk7);

  if HasSearchMove(aiMove) then
    Writeln('PASS: ', aiMove.pathRows[1], ' ', aiMove.pathCols[1])
  else
    Writeln('FAIL');
end;

begin
  Probe;
end.
