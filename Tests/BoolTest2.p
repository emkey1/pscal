program BoolTest2;
var
    a, b, result: boolean;
begin
    a := true;
    b := false;
    
    { Test AND }
    if (a and b) then
        writeln('a and b is true')
    else
        writeln('a and b is false');
    
    { Test OR }
    if (a or b) then
        writeln('a or b is true')
    else
        writeln('a or b is false');
    
    { Test NOT }
    if (not a) then
        writeln('not a is true')
    else
        writeln('not a is false');
    
    { Combine operators }
    result := (a and (not b)) or false;
    if result then
        writeln('Complex expression evaluated to true')
    else
        writeln('Complex expression evaluated to false');
end.

