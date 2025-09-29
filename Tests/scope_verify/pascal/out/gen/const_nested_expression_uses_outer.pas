program ConstNestedExpression;
const
  Base = 3;

procedure Report;
const
  Step = Base + 2;
begin
  writeln('step=', Step);
end;

begin
  Report;
  writeln('base=', Base);
end.