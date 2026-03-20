program OddBuiltinCondition;

var
  r, c: Integer;
  oddCount: Integer;

begin
  oddCount := 0;
  for r := 0 to 1 do
    for c := 0 to 2 do
      if odd(r + c) then
        oddCount := oddCount + 1;

  if (oddCount = 3) and odd(5) and (not odd(4)) then
    writeln('PASS: odd builtin');
end.
