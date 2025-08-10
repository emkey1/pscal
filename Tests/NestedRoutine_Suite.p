program NestedRoutine_Suite;

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

    function CharAt(i: integer): char;
    begin
        CharAt := outtext[i];
    end;

begin
    g := 5;
    currentline := 0;
    outtext := 'start';
    InnerProc;
    writeln('InnerFunc=', InnerFunc);
    writeln('currentline=', currentline);
    writeln('outtext=', outtext);
    writeln('CharAt(2)=', CharAt(2));
end;

begin
    g := 10;
    Outer;
    writeln('g=', g);
end.
