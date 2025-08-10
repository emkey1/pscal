program NestedVarArray;

procedure Outer(var arr: array[1..3] of integer);
  procedure Inner;
  begin
    arr[2] := 42;
  end;
begin
  Inner;
end;

var myArr: array[1..3] of integer;
var i: integer;

begin
  for i := 1 to 3 do
    myArr[i] := i;
  Outer(myArr);
  if myArr[2] = 42 then
    writeln('PASS: VAR array accessed in nested procedure')
  else
    writeln('FAIL: VAR array accessed in nested procedure');
end.
