program BenchIoHttp;
{ File I/O plus HTTP-over-file:// loopback (no network, no flakiness).
  Exercises text-file builtins, the HTTP session/request builtins and
  mstream churn.  Scratch files go in VM_BENCH_TMP (set by the runner)
  or the current directory. }

const
  FILE_ROUNDS = 360;
  FILE_LINES = 200;
  HTTP_ROUNDS = 1500;

var
  t0, t1: double;
  dir, path, url: string;
  checkResult: integer;

function FileKernel: integer;
var
  r, i, acc: integer;
  f: text;
  line: string;
begin
  acc := 0;
  for r := 1 to FILE_ROUNDS do
  begin
    assign(f, path);
    rewrite(f);
    for i := 1 to FILE_LINES do
      writeln(f, 'line ', i, ' payload abcdefghij');
    close(f);
    assign(f, path);
    reset(f);
    while not eof(f) do
    begin
      readln(f, line);
      acc := acc + length(line);
    end;
    close(f);
  end;
  FileKernel := acc;
end;

function HttpKernel: integer;
var
  r, s, code, acc: integer;
  ms: mstream;
begin
  acc := 0;
  s := HttpSession();
  for r := 1 to HTTP_ROUNDS do
  begin
    ms := mstreamcreate();
    code := HttpRequest(s, 'GET', url, nil, ms);
    acc := acc + code;
    mstreamfree(ms);
  end;
  HttpClose(s);
  HttpKernel := acc;
end;

begin
  dir := GetEnv('VM_BENCH_TMP');
  if dir = '' then
    dir := '.';
  path := dir + '/vm_bench_io_scratch.txt';

  t0 := RealTimeClock();
  checkResult := FileKernel;
  { The last written file doubles as the HTTP loopback payload. }
  if copy(path, 1, 1) = '/' then
    url := 'file://' + path
  else
    url := 'file://' + GetEnv('PWD') + '/' + path;
  checkResult := (checkResult + HttpKernel) mod 1000000007;
  t1 := RealTimeClock();
  writeln('check=', checkResult);
  writeln('elapsed_s=', (t1 - t0):0:6);
end.
