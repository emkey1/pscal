program SysUtilsFormatOpenArray;

uses
  SysUtils;

var
  x: Integer;
  s: String;

begin
  x := 7;
  s := Format('%3d', [x]);
  Writeln('single=', s);
  Writeln('multi=', Format('%s:%d:%c', ['maze', 42, '!']));
end.
