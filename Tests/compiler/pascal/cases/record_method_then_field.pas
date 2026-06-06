program RecordMethodThenField;

type
  TBaseSensor = record
    procedure Start;
    fName: string;
  end;

procedure TBaseSensor.Start;
begin
end;

var
  sensor: TBaseSensor;

begin
  sensor.fName := 'ok';
  sensor.Start;
  Writeln('PASS: ', sensor.fName);
end.
