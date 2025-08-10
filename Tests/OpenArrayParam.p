program OpenArrayParam;

procedure Proc(var a: array of integer);
begin
end;

const
  N = 5;

var
  arr: array[1..N] of integer;
begin
  Proc(arr);
end.
