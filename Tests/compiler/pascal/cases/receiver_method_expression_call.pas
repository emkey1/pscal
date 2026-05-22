program ReceiverMethodExpressionCall;

type
  PCounter = ^TCounter;
  TCounter = record
    value: Integer;
    function Next(delta: Integer): Integer;
  end;

function TCounter.Next(delta: Integer): Integer;
var
  selfPtr: PCounter;
begin
  selfPtr := myself;
  selfPtr^.value := selfPtr^.value + delta;
  Next := selfPtr^.value;
end;

var
  counter: PCounter;
  value: Integer;
begin
  new(counter);
  counter^.value := 40;
  value := counter.Next(2);
  if value = 42 then
    writeln('PASS: receiver method expression call')
  else
    writeln('FAIL: receiver method expression call = ', value);
end.
