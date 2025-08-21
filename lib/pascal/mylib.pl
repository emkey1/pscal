unit mylib;

interface
    { Exported procedures and functions }
    procedure Greet(name: string);
    function Add(a, b: integer): integer;
    function GetPi: real;

    { Exported type }
    type
        TPerson = record
            name: string;
            age: integer;
        end;

    { Exported variable }
    var
        GlobalCounter: integer;

implementation

procedure Greet(name: string);
begin
    writeln('Hello, ', name, '!');
end;

function Add(a, b: integer): integer;
begin
    Add := a + b;
end;

function GetPi: real;
begin
    GetPi := 3.14159;
end;

{ Initialization code for the unit }
begin
    GlobalCounter := 0;
end.

