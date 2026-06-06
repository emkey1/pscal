program UnicodeTextRuntimeOps;

var
  s: UnicodeString;
  c: WideChar;
  outS: UnicodeString;
begin
  s := '雪山';
  c := s[2];
  Writeln('index=', c);
  Writeln('indexord=', Ord(c));
  Writeln('pos=', Pos('山', s));
  Writeln('repeat=', StringOfChar('雪', 2));
  outS := '';
  Str(c, outS);
  Writeln('str=', outS);
end.
