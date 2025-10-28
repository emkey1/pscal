program ClosureCaptureEscapeError;

type
  TProc = procedure();

var
  Stored: TProc;

procedure Register;
var
  Value: integer;

  procedure Inner;
  begin
    Value := Value + 1;
    writeln('inner value=', Value);
  end;

begin
  Value := 0;
  Stored := @Inner;
end;

begin
  Register;
end.
