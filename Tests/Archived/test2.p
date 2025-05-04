program TestRealLoop;
var
    x: real;
begin
    x := 0;
    while x < 3 do
    begin
        writeln('x = ', x);
        x := x + 0.75;
    end;
end.
