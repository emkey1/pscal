program ArrayArgumentMismatch;

procedure Proc(var a: array[1..10] of integer; n: integer);
begin
end;

var
  a: array[1..10] of integer;
  n: integer;
begin
  n := 3;
  Proc(n, a);
end.
