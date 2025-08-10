program OpenArrayBaseTypeMismatch;

procedure Proc(var a: array of integer);
begin
end;

var
  arr: array[1..5] of real;
begin
  Proc(arr);
end.
