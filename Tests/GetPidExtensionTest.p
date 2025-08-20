program GetPidExtensionTest;

var
  pid: integer;
begin
  pid := GetPid();
  if pid <= 0 then
  begin
    writeln('GetPid returned invalid PID: ', pid);
    halt(1);
  end;
  writeln('GetPid extension test passed: ', pid);
end.
