program UnicodeCompareAndCase;

var
  c: WideChar;
  s: UnicodeString;
begin
  c := '雪';
  s := '雪';
  if c = s then
    Writeln('eq=1')
  else
    Writeln('eq=0');

  if '雪山' > '雪' then
    Writeln('gt=1')
  else
    Writeln('gt=0');

  case c of
    '山': Writeln('case=bad');
    '雪': Writeln('case=snow');
  else
    Writeln('case=other');
  end;
end.
