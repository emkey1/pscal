program FileIOEdgeTests;

var
  f: Text;
  s: string;
  i: integer;
  c: char;
  r: real;
  ok: boolean;
  e: integer;

procedure verify(ok: boolean; test: string);
begin
  write('Test: ', test, ' ... ');
  if ok then
    writeln('PASS')
  else
    writeln('FAIL');
end;

begin
  writeln('--- Starting File I/O Edge Tests ---');

  assign(f, 'edge_input.txt');
  rewrite(f);
  writeln(f, '');
  writeln(f, '   99    3.14   Z   hello there');
  writeln(f, '42abc');
  writeln(f, '      ');
  writeln(f, 'Z');
  close(f);
  e := ioresult;
  verify(e = 0, 'Setup: create edge_input.txt');

  assign(f, 'edge_input.txt');
  reset(f);
  e := ioresult;
  verify(e = 0, 'Reset edge_input.txt');
  ok := e = 0;

  if ok then
  begin
    readln(f, s);
    e := ioresult;
    ok := (e = 0) and (s = '');
    verify(ok, 'Empty line -> s=""');
  end;

  if ok then
  begin
    i := -1;
    r := -1.0;
    c := #0;
    s := '<<unset>>';
    readln(f, i, r, c, s);
    e := ioresult;
    ok := (e = 0) and (i = 99) and (abs(r - 3.14) < 0.000001)
      and (c = 'Z') and (s = 'hello there');
    if not ok then
    begin
      writeln('  parse details:');
      if e <> 0 then writeln('    ioresult expected 0, got ', e);
      if i <> 99 then writeln('    i expected 99, got ', i);
      if abs(r - 3.14) >= 0.000001 then writeln('    r expected ~3.14, got ', r:0:6);
      if c <> 'Z' then writeln('    c expected Z, got ', c);
      if s <> 'hello there' then writeln('    s expected "hello there", got "', s, '"');
    end;
    verify(ok, 'Mixed types parse (i=99, r~3.14, c=Z, s="hello there")');
  end;

  if ok then
  begin
    i := -1;
    s := '<<unset>>';
    readln(f, i, s);
    e := ioresult;
    ok := (e = 0) and (i = 42) and (s = 'abc');
    verify(ok, 'Token split without space (i=42, s="abc")');
  end;

  if ok then
  begin
    i := 123;
    c := 'X';
    s := 'kept';
    readln(f, i, c, s);
    e := ioresult;
    ok := e <> 0;
    verify(ok, 'Spaces-only line triggers IOResult<>0');
  end;

  if ok then
  begin
    c := #0;
    readln(f, c);
    e := ioresult;
    ok := (e = 0) and (c = 'Z');
    verify(ok, 'Single char line -> c=Z');
  end;

  close(f);
  e := ioresult;
  verify(e = 0, 'Close edge_input.txt');
  writeln('--- Edge Tests Complete ---');
end.
