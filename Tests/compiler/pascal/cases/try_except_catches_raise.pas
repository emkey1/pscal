program TryExceptCatchesRaise;

function ComputeMoveCount(shouldFail: boolean): integer;
begin
  if shouldFail then
    raise 1;
  ComputeMoveCount := 3;
end;

function HasAnyMove(shouldFail: boolean): boolean;
begin
  try
    HasAnyMove := (ComputeMoveCount(shouldFail) > 0);
  except
    HasAnyMove := false;
  end;
end;

begin
  if HasAnyMove(false) and (not HasAnyMove(true)) then
    writeln('PASS: Pascal try/except catches raised errors');
end.
