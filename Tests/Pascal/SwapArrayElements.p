program SwapArrayElements;

procedure swap(var x, y: integer);
var temp: integer;
begin
  temp := x;
  x := y;
  y := temp;
end;

var
  arr: array[1..10] of integer;
  i, j: integer;
begin
  i := 1;
  j := 2;
  arr[i] := 10;
  arr[j] := 20;
  swap(arr[i], arr[j]);
end.
