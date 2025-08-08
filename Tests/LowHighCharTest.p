program LowHighCharTest;

procedure AssertEqualChar(expected, actual: char; testName: string);
begin
  write('START: ', testName, ': ');
  if expected = actual then
    writeln('PASS')
  else
    writeln('FAIL (expected: ''', expected, ''', got: ''', actual, ''')');
end;

begin
  AssertEqualChar(chr(0), low(char), 'low(char)');
  AssertEqualChar(chr(255), high(char), 'high(char)');
end.
