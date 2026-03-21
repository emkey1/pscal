program ValBuiltinVarArgs;

procedure CheckLocalVal;
var
  parsed: Integer;
  code: Integer;
begin
  Val('42', parsed, code);

  if (parsed = 42) and (code = 0) then
    Writeln('PASS: val builtin var args')
  else
    Writeln('FAIL: parsed=', parsed, ' code=', code);
end;

begin
  CheckLocalVal;
end.
