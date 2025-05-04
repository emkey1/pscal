program BooleanTest;

var
  a, b, c, result: boolean;

begin
  a := true;
  b := false;
  c := true;

  writeln('a = ', a);        { true }
  writeln('b = ', b);        { false }
  writeln('c = ', c);        { true }

  result := a and c;
  writeln('a and c = ', result);    { true }

  result := a and b;
  writeln('a and b = ', result);    { false }

  result := a or b;
  writeln('a or b = ', result);     { true }

  result := not b;
  writeln('not b = ', result);      { true }

  result := not (a and c);
  writeln('not (a and c) = ', result);  { false }

  result := (a or b) and (not c);
  writeln('(a or b) and (not c) = ', result);  { false }

  result := (a and not b) or (c and b);
  writeln('(a and not b) or (c and b) = ', result);  { true }
end.
// Correct results below...
// a = true
// b = false
// c = true
// a and c = true
// a and b = false
// a or b = true
// not b = true
// not (a and c) = false
// (a or b) and (not c) = false
// (a and not b) or (c and b) = true
