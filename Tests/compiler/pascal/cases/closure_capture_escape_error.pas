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
    if Value = 2 then
      writeln('PASS: capturing closure escapes and retains state')
    else
      writeln('inner value=', Value);
  end;

begin
  Value := 0;
  Stored := @Inner;
end;

begin
  Register;
  Stored();
  Stored();
end.
