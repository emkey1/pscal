program NestedRoutineAccessTest;

var g: integer;

procedure Outer;
    var currentline: integer;
        outtext: string[20];

    procedure InnerProc;
    begin
        g := g + 1;
        currentline := currentline + 1;
        outtext := 'changed';
    end;

    function InnerFunc: integer;
    begin
        InnerProc;
        InnerFunc := g + currentline;
    end;

begin
    g := 5;
    currentline := 0;
    outtext := 'start';
    InnerProc;
    if InnerFunc = g + currentline then
        writeln('PASS: Nested routines can access outer vars')
    else
        writeln('FAIL: Nested routines cannot access outer vars');
    writeln('currentline=', currentline);
    writeln('outtext=', outtext);
end;

begin
    g := 10;
    Outer;
end.
