program ClosureCaptureReturnStore;

type
  TStep = function(): integer;

var
  Stored: TStep;

function MakeCounter(start: integer): TStep;
var
  value: integer;
  function Next: integer;
  begin
    value := value + 1;
    Next := value;
  end;
begin
  value := start;
  MakeCounter := @Next;
end;

procedure StoreCounter(start: integer);
begin
  Stored := MakeCounter(start);
end;

var
  counter: TStep;
  first, second, third, fourth: integer;
begin
  counter := MakeCounter(10);
  first := counter();
  second := counter();
  StoreCounter(2);
  third := Stored();
  fourth := Stored();
  if (first <> 11) or (second <> 12) or (third <> 3) or (fourth <> 4) then
    writeln('FAIL: unexpected closure results ', first, ' ', second, ' ', third, ' ', fourth)
  else
    writeln('PASS: capturing closure stored and returned');
end.
