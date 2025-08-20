program ExitEarlyTest;

function Foo(): integer;
begin
  writeln('Inside Foo');
  exit;
  Foo := 1; { This line should not execute }
end;

begin
  writeln('Start');
  Foo;
  writeln('After Foo');
end.
