program BreakLoopTest;
uses CRT; // Include CRT for WriteLn

var
  i: integer;
  ch: char;

begin
  WriteLn('Testing FOR loop break...');
  for i := 1 to 10 do
  begin
    Write(i:3);
    if i = 5 then
    begin
      WriteLn; // Newline before break message
      WriteLn('Breaking FOR loop at i = ', i);
      Break;
    end;
  end;
  WriteLn('After FOR loop.');
  WriteLn;

  WriteLn('Testing WHILE loop break...');
  i := 0;
  while i < 10 do
  begin
    i := i + 1;
    Write(i:3);
    if i = 6 then
    begin
      WriteLn; // Newline before break message
      WriteLn('Breaking WHILE loop at i = ', i);
      Break;
    end;
  end;
  WriteLn('After WHILE loop.');
  WriteLn;

  WriteLn('Testing REPEAT loop break...');
  i := 0;
  repeat
    i := i + 1;
    Write(i:3);
    if i = 7 then
    begin
       WriteLn; // Newline before break message
       WriteLn('Breaking REPEAT loop at i = ', i);
       Break;
    end;
  until i >= 10; // Condition would normally stop it at 10
  WriteLn('After REPEAT loop.');
  WriteLn;

end.
