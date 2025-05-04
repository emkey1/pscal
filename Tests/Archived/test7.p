program TestProcedures;
var
    a, b, c: integer;
    
procedure AddNumbers(x, y: integer);
begin
    c := x + y;
    writeln('Sum is: ', c);
end;

begin
    a := 7;
    b := 8;
    AddNumbers(a, b);
end.
