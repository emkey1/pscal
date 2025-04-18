program CaseRangeTest;
var
    n: integer;
begin
    for n := 1 to 6 do
    begin
        write('n=', n, ' => ');
        case n of
            1: writeln('one');
            2..4: writeln('two to four');
            5: writeln('five');
        else
            writeln('unknown');
        end;
    end;
end.

