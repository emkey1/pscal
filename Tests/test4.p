program TestRepeat;
var
    count: integer;
begin
    count := 0;
    repeat
        count := count + 1;
        writeln('Count = ', count);
    until count = 5;
end.
