program UnicodePrintfFormat;

var
  fmt: UnicodeString;
  value: UnicodeString;
begin
  fmt := 'fmt=%s\n';
  value := '雪';
  Printf(fmt, value);
end.
