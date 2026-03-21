program ContinueStatementLoops;

var
  i, sumWhile, sumRepeat, sumFor: Integer;
begin
  i := 0;
  sumWhile := 0;
  while i < 6 do
  begin
    Inc(i);
    if (i mod 2) = 0 then
      continue;
    sumWhile := sumWhile + i;
  end;

  i := 0;
  sumRepeat := 0;
  repeat
    Inc(i);
    if i = 3 then
      continue;
    sumRepeat := sumRepeat + i;
  until i >= 4;

  sumFor := 0;
  for i := 1 to 5 do
  begin
    if i = 4 then
      continue;
    sumFor := sumFor + i;
  end;

  if (sumWhile = 9) and (sumRepeat = 7) and (sumFor = 11) then
    Writeln('PASS: continue statement loops')
  else
    Writeln('FAIL: ', sumWhile, ' ', sumRepeat, ' ', sumFor);
end.
