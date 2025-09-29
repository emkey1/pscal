program ConstShadow;
const
  Base = 2;

procedure Show;
const
  Base = 5;
begin
  writeln('inner=', Base);
end;

begin
  Show;
  writeln('outer=', Base);
end.