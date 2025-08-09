program EnumSuccTest;

type
  TColor = (cRed, cGreen, cBlue, cYellow);

procedure AssertEqualEnum(expected, actual: TColor; testName: string);
begin
  if ord(expected) = ord(actual) then
    writeln(testName, ': PASS')
  else
    writeln(testName, ': FAIL (expected ', ord(expected), ', got ', ord(actual), ')');
end;

begin
  AssertEqualEnum(cGreen, succ(cRed), 'Succ(cRed)');
  AssertEqualEnum(cYellow, succ(cBlue), 'Succ(cBlue)');
end.
