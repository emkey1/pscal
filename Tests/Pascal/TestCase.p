program CaseTest;
var
  x: integer;
begin
  x := 2;
  case x of
    1: writeln('One');
    2: writeln('Two');
    3: writeln('Three');
    else writeln('Other');
  end;
  writeln('X = ', x);
end.
