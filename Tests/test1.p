program TestArithmetic;
var
    a, b, c: integer;
begin
    a := 5;
    b := 10;
    c := a * b - 3;   { Expected: 5*10-3 = 47 }
    if c > 40 then
        writeln('c is greater than 40')
    else
        writeln('c is less than or equal to 40');
end.
