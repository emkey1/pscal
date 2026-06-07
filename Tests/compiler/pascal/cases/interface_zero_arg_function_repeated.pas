program InterfaceZeroArgFunctionRepeated;

type
  ICounter = interface
    function GetValue: Integer;
  end;

  TCounter = record
    value: Integer;
    function GetValue: Integer; virtual;
  end;

function TCounter.GetValue: Integer;
begin
  GetValue := value;
end;

procedure RunTest;
var
  counter: ^TCounter;
  iface: ICounter;
  i: Integer;
  total: Integer;
begin
  new(counter);
  counter^.value := 7;
  iface := ICounter(counter);
  total := 0;

  for i := 1 to 2000 do
    total := total + iface.GetValue();

  if total <> 14000 then
    writeln('FAIL: repeated interface zero-arg function dispatch');

  dispose(counter);
  writeln('PASS: repeated interface zero-arg function dispatch');
end;

begin
  RunTest;
end.
