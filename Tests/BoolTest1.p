program BoolTest1;
var
    flag: boolean;
begin
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
end.

