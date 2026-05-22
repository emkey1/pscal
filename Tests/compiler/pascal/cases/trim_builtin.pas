program TrimBuiltin;

var
  value: string;
begin
  value := Trim('   padded value   ');
  if value = 'padded value' then
    writeln('PASS: trim builtin')
  else
    writeln('FAIL: trim builtin = >', value, '<');
end.
