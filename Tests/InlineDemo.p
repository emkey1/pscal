program InlineDemo;

// The 'inline' directive is a hint to the compiler.
function AddOne(x: integer): integer; inline;
begin
  Result := x + 1;
end;

var
  a, b: integer;

begin
  a := 5;

  // With inlining, the compiler would try to replace this call
  // with the code 'b := a + 1;' directly.
  b := AddOne(a);

  WriteLn('The result is: ', b);
end.
