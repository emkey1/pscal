program UnicodeTextSemantics;

var
  s: UnicodeString;
  c: WideChar;
begin
  s := '雪山';
  c := '雪';
  Writeln('len=', Length(s));
  Writeln('copy=', Copy(s, 2, 1));
  Writeln('ord=', Ord(c));
end.
