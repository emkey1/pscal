program LocalBuiltinShadowReference;

function PrefixOrEmpty(const value: string): string;
var
  ln: integer;
begin
  ln := Length(value);
  if ln > 0 then
    PrefixOrEmpty := Copy(value, 1, ln)
  else
    PrefixOrEmpty := '';
end;

begin
  if PrefixOrEmpty('abc') = 'abc' then
    writeln('PASS: local builtin shadow reference')
  else
    writeln('FAIL: local builtin shadow reference');
end.
