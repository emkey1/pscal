program TestForLoop;
var
    i: integer;
begin
    { For loop with "to" }
    for i := 1 to 5 do
        writeln('For loop (TO): i = ', i);

    { For loop with "downto" }
    for i := 5 downto 1 do
        writeln('For loop (DOWNTO): i = ', i);
end.
