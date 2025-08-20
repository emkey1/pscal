program PrimitiveArgCall;

procedure Foo(var i: integer; var b: boolean);
begin
end;

var
  x: integer;
  y: boolean;
begin
  x := 10;
  y := false;
  Foo(x, y);
end.
