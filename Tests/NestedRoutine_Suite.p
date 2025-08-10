program NestedRoutine_Suite;

var g: integer;

procedure Outer;

    procedure InnerProc;
    begin
        g := g + 1;
    end;

    function InnerFunc: integer;
    begin
        InnerProc;
        InnerFunc := g;
    end;

begin
    g := 5;
    InnerProc;
    writeln('InnerFunc=', InnerFunc);
end;

begin
    g := 10;
    Outer;
    writeln('g=', g);
end.
