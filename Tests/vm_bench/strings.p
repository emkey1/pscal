program BenchStrings;
{ String concatenation and manipulation: repeated builds, copy, pos,
  comparisons.  Exercises string alloc/copy/free churn in the VM. }

const
  ROUNDS = 1500;
  PIECES = 200;

var
  t0, t1: double;
  checkResult: integer;

function Kernel: integer;
var
  r, i, acc, hits: integer;
  s, chunk: string;
begin
  acc := 0;
  hits := 0;
  for r := 1 to ROUNDS do
  begin
    s := '';
    for i := 1 to PIECES do
    begin
      chunk := 'ab' + chr(ord('a') + (i mod 26));
      s := s + chunk;
    end;
    acc := acc + length(s);
    if pos('abz', s) > 0 then
      hits := hits + 1;
    s := copy(s, 10, 50) + upcase(copy(s, 1, 5));
    if s > 'a' then
      acc := acc + length(s);
  end;
  Kernel := acc * 10 + hits;
end;

begin
  t0 := RealTimeClock();
  checkResult := Kernel;
  t1 := RealTimeClock();
  writeln('check=', checkResult);
  writeln('elapsed_s=', (t1 - t0):0:6);
end.
