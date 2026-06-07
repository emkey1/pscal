program RecordInheritEmbeddedBase;

type
  PBaseSensor = ^TBaseSensor;
  TBaseSensor = record
    value: Integer;
    procedure Bump;
    function ReadValue: Integer;
  end;

  TTempSensor = record
    inherit TBaseSensor;
    seed: Integer;
  end;

procedure TBaseSensor.Bump;
var
  selfPtr: PBaseSensor;
begin
  selfPtr := myself;
  selfPtr^.value := selfPtr^.value + 1;
end;

function TBaseSensor.ReadValue: Integer;
var
  selfPtr: PBaseSensor;
begin
  selfPtr := myself;
  ReadValue := selfPtr^.value;
end;

var
  sensor: TTempSensor;

begin
  sensor.value := 40;
  sensor.seed := 7;
  sensor.Bump;
  Writeln('PASS: ', sensor.ReadValue(), ' ', sensor.seed);
end.
