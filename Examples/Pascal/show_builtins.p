program ShowBuiltins;

type PInt = ^integer;
var
  a, b: PInt;
  pid: integer;
begin
  pid := GetPid();
  writeln('PID = ', pid);
  New(a); New(b);
  a^ := 1;
  b^ := 2;
  Swap(a, b);
  writeln('After Swap: a=', a^, ' b=', b^);
  Dispose(a); Dispose(b);
end.
