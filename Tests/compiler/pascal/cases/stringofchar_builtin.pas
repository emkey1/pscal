program StringOfCharBuiltin;

var
  border: String;

begin
  border := StringOfChar('-', 5);

  if (border = '-----') and
     (Length(StringOfChar('x', 3)) = 3) and
     (Copy(StringOfChar('z', 2), 1, 2) = 'zz') then
    writeln('PASS: stringofchar builtin');
end.
