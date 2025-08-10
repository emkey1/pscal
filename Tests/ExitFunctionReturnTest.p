program ExitFunctionReturnTest;

function Flag(): boolean;
begin
  Flag := true;
  exit;
  Flag := false; { unreachable }
end;

begin
  if Flag() then
    writeln('ok')
  else
    writeln('fail');
end.
