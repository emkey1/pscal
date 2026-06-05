#!/usr/bin/env pascal
program InputKeyProbe;

uses CRT;

var
  ch: Char;
  idx: Integer;
  line: string;

begin
  ClrScr;
  WriteLn('InputKeyProbe');
  WriteLn('Press keys. ESC twice exits. Enter clears current buffer.');
  WriteLn('---');
  line := '';
  idx := 0;
  while True do
  begin
    ch := ReadKey;
    idx := idx + 1;
    WriteLn('#', idx:1, ' ord=', Ord(ch):1, ' char="', ch, '"');

    if ch = #27 then
    begin
      if line = #27 then
      begin
        WriteLn('Exit.');
        Break;
      end
      else
        line := #27;
    end
    else if ch = #13 then
    begin
      line := '';
      WriteLn('--- line reset ---');
    end
    else
      line := line + ch;
  end;
end.
