program BenchArith;
{ Tight arithmetic loop on locals: add/mul/div/mod, int + real mix.
  Exercises raw dispatch + numeric opcode cost. }

const
  OUTER = 4;
  INNER = 100000;

var
  t0, t1: double;
  checkResult: integer;

function Kernel: integer;
var
  i, j, acc, x: integer;
  r: double;
begin
  acc := 0;
  r := 1.0;
  for i := 1 to OUTER do
  begin
    x := i;
    for j := 1 to INNER do
    begin
      acc := acc + j;
      x := (x * 31 + j) mod 65521;
      if (j and 1023) = 0 then
        r := r + acc / (x + 1);
    end;
    acc := acc mod 1000000007;
  end;
  Kernel := (acc + x) mod 1000000007 + trunc(r) mod 97;
end;

begin
  t0 := RealTimeClock();
  checkResult := Kernel;
  t1 := RealTimeClock();
  writeln('check=', checkResult);
  writeln('elapsed_s=', (t1 - t0):0:6);
end.
