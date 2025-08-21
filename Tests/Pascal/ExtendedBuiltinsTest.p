program ExtendedBuiltinsTest;

type PInt = ^integer;
var
  pid: integer;
  a, b: PInt;
begin
  pid := GetPid();
  if pid <= 0 then
  begin
    writeln('GetPid returned invalid PID: ', pid);
    halt(1);
  end;

  New(a); New(b);
  a^ := 1;
  b^ := 2;
  Swap(a, b);
  if (a^ <> 2) or (b^ <> 1) then
  begin
    writeln('Swap failed: a=', a^, ' b=', b^);
    halt(1);
  end;

  if not FileExists('Pascal/ExtendedBuiltinsTest.p') then
  begin
    writeln('FileExists failed to detect existing file');
    halt(1);
  end;
  if FileExists('Pascal/nonexistent.file') then
  begin
    writeln('FileExists falsely reported missing file');
    halt(1);
  end;

  writeln('Extended builtins test passed: pid=', pid, ' a=', a^, ' b=', b^);
  Dispose(a); Dispose(b);
end.
