program SimpleMath;
var
  a, b, result: Integer;
begin
  a := 10;
  b := 5 + (2 * 3); // 5 + 6 = 11
  result := a + b - 1; // 10 + 11 - 1 = 20
  WriteLn('Result should be 20: ', result);
  WriteLn('a = ', a, ', b = ', b);
end.
