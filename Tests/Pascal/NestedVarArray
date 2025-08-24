program NestedVarArray;

procedure Level1(var arr: array[1..3] of integer);
  procedure Level2(var arr2: array[1..3] of integer);
    procedure Level3;
    begin
      arr2[3] := 99;
    end;
  begin
    Level3;
  end;
begin
  Level2(arr);
end;

var myArr: array[1..3] of integer;
var i: integer;

begin
  for i := 1 to 3 do
    myArr[i] := i;
  Level1(myArr);
  if myArr[3] = 99 then
    writeln('PASS: VAR array accessed through multiple nested procedures')
  else
    writeln('FAIL: VAR array accessed through multiple nested procedures');
end.
