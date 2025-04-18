program BoolTest4;
var
    i: integer;
    loopActive: boolean;
begin
    i := 1;
    loopActive := true;
    while loopActive do begin
        writeln('Iteration: ', i);
        i := i + 1;
        { Stop looping when i > 5 }
        if i > 5 then
            loopActive := false;
    end;
end.
