program EnumCaseTest;
type
  Color = (Red, Green, Blue);
const
  DefaultColor = Green;
var
  c, d: Color;
begin
  c := Red;
  d := DefaultColor;
  if d = Green then writeln('Assigned enum constant');
  if c <> d then writeln('Enums differ');
  case c of
    Red: writeln('Red branch');
    Green: writeln('Green branch');
    Blue: writeln('Blue branch');
  end;
end.
