program BoolTest3;
var
    x: integer;
    cond1, cond2: boolean;
begin
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
end.

