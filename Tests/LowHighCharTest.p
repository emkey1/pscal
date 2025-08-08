program LowHighCharTest;

type
  TColor = (Red, Green, Blue);

procedure AssertEqualChar(expected, actual: char; testName: string);
begin
  write('START: ', testName, ': ');
  if expected = actual then
    writeln('PASS')
  else
    writeln('FAIL (expected: ''', expected, ''', got: ''', actual, ''')');
end;

procedure AssertEqualColor(expected, actual: TColor; testName: string);
begin
  write('START: ', testName, ': ');
  if ord(expected) = ord(actual) then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', ord(expected), ', got: ', ord(actual), ')');
end;

var
  cLow, cHigh: TColor;

begin
  AssertEqualChar(chr(0), low(char), 'low(char)');
  AssertEqualChar(chr(255), high(char), 'high(char)');
  cLow := low(TColor);
  cHigh := high(TColor);
  AssertEqualColor(Red, cLow, 'low(TColor)');
  AssertEqualColor(Blue, cHigh, 'high(TColor)');
end.
