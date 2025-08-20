program TestMath;

var
  a, b, c: real;
  i: integer;

begin
  writeln('--- Math Functions Test ---');
  
  { Test sin and cos }
  a := 1.5708;  { approximately π/2 }
  writeln('sin(', a, ') = ', sin(a));
  writeln('cos(', a, ') = ', cos(a));
  
  { Test tan }
  b := 0.7854;  { approximately π/4 }
  writeln('tan(', b, ') = ', tan(b));
  
  { Test sqrt }
  c := 16.0;
  writeln('sqrt(', c, ') = ', sqrt(c));
  
  { Test ln }
  a := 2.71828;  { approximate value of e }
  writeln('ln(', a, ') = ', ln(a));
  
  { Test exp }
  a := 1.0;
  writeln('exp(', a, ') = ', exp(a));
  
  { Test abs on an integer and on a real }
  i := -42;
  writeln('abs(', i, ') = ', abs(i));
  a := -3.1416;
  writeln('abs(', a, ') = ', abs(a));
  
  writeln('--- End of Math Functions Test ---');
end.

