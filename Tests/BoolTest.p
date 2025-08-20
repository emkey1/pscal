program BoolTest;

var
  flag, a, b, result, cond1, cond2, loopActive: boolean;
  x, i: integer;

begin
  { Basic boolean assignment }
  flag := true;
  if flag then
    writeln('flag is true')
  else
    writeln('flag is false');

  flag := false;
  if flag then
    writeln('flag is true')
  else
    writeln('flag is false');

  { Boolean operators }
  a := true;
  b := false;
  if (a and b) then
    writeln('a and b is true')
  else
    writeln('a and b is false');

  if (a or b) then
    writeln('a or b is true')
  else
    writeln('a or b is false');

  if (not a) then
    writeln('not a is true')
  else
    writeln('not a is false');

  result := (a and (not b)) or false;
  if result then
    writeln('Complex expression evaluated to true')
  else
    writeln('Complex expression evaluated to false');

  { Relational expressions }
  x := 10;
  cond1 := (x > 5);
  if cond1 then
    writeln('x > 5 is true')
  else
    writeln('x > 5 is false');

  cond2 := (x < 5) or (x = 10);
  if cond2 then
    writeln('(x < 5) or (x = 10) is true')
  else
    writeln('(x < 5) or (x = 10) is false');

  { Boolean-controlled loop }
  i := 1;
  loopActive := true;
  while loopActive do
  begin
    writeln('Iteration: ', i);
    i := i + 1;
    if i > 5 then
      loopActive := false;
  end;
end.
