program EnumComparisonTest;
type
  Color = (Red, Green, Blue);
var
  a, b: Color;
begin
  a := Red;
  b := Green;
  if a = Red then writeln('Color a is Red');
  if a <> b then writeln('Enums compare not equal');
  if b > a then writeln('Enums compare greater');
end.
