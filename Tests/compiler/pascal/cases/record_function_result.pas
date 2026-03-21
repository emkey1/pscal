program RecordFunctionResult;

type
  Pair = record
    leftValue, rightValue: Integer;
  end;

function EmptyPair: Pair;
begin
  EmptyPair.leftValue := 0;
  EmptyPair.rightValue := 1;
end;

function GetPair(useExit: Boolean): Pair;
begin
  if useExit then
    Exit(EmptyPair);
  GetPair := EmptyPair;
end;

var
  p: Pair;
begin
  p := GetPair(true);
  if (p.leftValue = 0) and (p.rightValue = 1) then
    Writeln('PASS: record function result')
  else
    Writeln('FAIL: record function result');
end.
