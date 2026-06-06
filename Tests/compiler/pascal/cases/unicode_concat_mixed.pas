program UnicodeConcatMixed;

var
  s: UnicodeString;
  c: WideChar;
begin
  c := '雪';
  s := c + '山';
  Writeln('len=', Length(s));
  Writeln('value=', s);
end.
