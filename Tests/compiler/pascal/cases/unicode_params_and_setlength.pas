program UnicodeParamsAndSetLength;

function EchoText(s: UnicodeString): UnicodeString;
begin
  EchoText := s;
end;

procedure ShowValues(c: WideChar; s: UnicodeString);
begin
  Writeln('char=', c);
  Writeln('text=', s);
end;

var
  s: UnicodeString;
  c: WideChar;
begin
  s := '雪山';
  c := EchoText('雪');
  ShowValues(c, EchoText(s));
  SetLength(s, 1);
  Writeln('trim=', s);
  SetLength(s, 3);
  Writeln('grow=', s, '.');
end.
