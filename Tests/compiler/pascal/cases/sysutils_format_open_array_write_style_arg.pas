program SysUtilsFormatOpenArrayWriteStyleArg;

uses
  SysUtils;

var
  sensorName: String;
  reading: Double;
  s: String;

begin
  sensorName := 'maze';
  reading := 3.5;
  s := Format('[%s] = %.2f', [sensorName, reading:0:2]);
  Writeln(s);
end.
