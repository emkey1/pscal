program ReadlnStringTest;

var
  f: Text;
  s: string;

begin
  assign(f, 'readln_test.txt');
  rewrite(f);
  writeln(f, 'Test');
  close(f);

  assign(f, 'readln_test.txt');
  reset(f);
  readln(f, s);
  close(f);

  if s = 'Test' then
    writeln('PASS')
  else
    writeln('FAIL: ', s);
end.

