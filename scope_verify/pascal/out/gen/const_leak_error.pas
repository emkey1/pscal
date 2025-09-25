program ConstLeak;

procedure Maker;
const
  Hidden = 4;
begin
  writeln('hidden=', Hidden);
end;

begin
  Maker;
  writeln(Hidden);
end.