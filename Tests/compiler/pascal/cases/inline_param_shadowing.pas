program InlineParamShadowing;

function IsValidPos(r, c: Integer): Boolean; inline;
begin
  IsValidPos := (r >= 1) and (r <= 8) and (c >= 1) and (c <= 8);
end;

procedure Probe(var ok: Boolean; r, c: Integer);
begin
  ok := IsValidPos(r, c);
end;

var
  ok: Boolean;
begin
  ok := false;
  Probe(ok, 5, 2);
  if ok then
    Writeln('PASS: inline parameter shadowing')
  else
    Writeln('FAIL: inline parameter shadowing');
end.
