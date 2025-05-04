program TestCombined;
var
    a, b, c: integer;
    x: real;
    greeting: string;
begin
    a := 10;
    b := 20;
    c := a + b;
    writeln('Sum of a and b is ', c);

    if c > 25 then
        writeln('c is greater than 25')
    else
        writeln('c is less than or equal to 25');

    x := 0.5;
    while x < 3.0 do
    begin
        writeln('x = ', x);
        x := x + 0.5;
    end;

    for a := 1 to 3 do
        writeln('Loop a = ', a);

    write('Enter a greeting: ');
    readln(greeting);
    writeln('You said: ', greeting);
end.
