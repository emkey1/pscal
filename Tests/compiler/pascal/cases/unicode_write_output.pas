program UnicodeWriteOutput;

begin
  Writeln('snow=', '雪');
  Writeln('block=', Chr(219));
  Writeln('blocks=', StringOfChar(Chr(219), 2));
end.
