program InlineExitInNestedHelper;

function Outer(n: Integer): Integer;
  function Helper(x: Integer): Boolean; inline;
  begin
    if x > 0 then
      Exit(true);
    Helper := false;
  end;
begin
  Outer := 7;
  if Helper(n) then
    Outer := 42;
end;

begin
  Writeln('PASS: inline exit = ', Outer(1), ' / ', Outer(0));
end.
