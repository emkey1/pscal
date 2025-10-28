program ClosureNonCapturingCallOk;

type
  TProc = procedure();

var
  Stored: TProc;

procedure Register;
  procedure Inner;
  begin
    writeln('PASS: non-capturing closure stored');
  end;
begin
  Inner;
end;

begin
  Register;
end.
