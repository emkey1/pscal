program SpawnAnonymousProcedureCapture;

var
  gResult: Integer;

procedure RunTest;
var
  tid: Integer;
  base: Integer;
begin
  base := 41;
  tid := spawn(procedure
               begin
                 gResult := base + 1;
               end);
  join(tid);
end;

begin
  gResult := 0;
  RunTest;
  Writeln('PASS: ', gResult);
end.
