program ArgumentTypeMismatch;

procedure Foo(i: integer; r: real);
begin
end;

var
  x: integer;
begin
  x := 5;
  Foo(x, x); { wrong type for second argument }
end.
