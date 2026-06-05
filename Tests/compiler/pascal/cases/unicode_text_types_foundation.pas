program UnicodeTextTypesFoundation;

var
  s: UnicodeString;
  c: WideChar;
begin
  s := 'hello';
  c := 'A';
  Writeln('PASS: unicode text foundation ', Length(s), ' ', Ord(c));
end.
