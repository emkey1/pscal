program UnicodeConstConcat;

const
  Mixed = '雪' + '山';
  Mixed2 = '雪' + #$2192;

begin
  Writeln('mixed=', Mixed);
  Writeln('len=', Length(Mixed));
  Writeln('mixed2=', Mixed2);
end.
