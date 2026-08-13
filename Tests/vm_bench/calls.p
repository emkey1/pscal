program BenchCalls;
{ Call-heavy recursion: naive Fibonacci plus mutually recursive parity.
  Exercises CALL/RETURN, frame setup, and parameter passing. }

var
  t0, t1: double;
  checkResult: integer;

function Fib(n: integer): integer;
begin
  if n < 2 then
    Fib := n
  else
    Fib := Fib(n - 1) + Fib(n - 2);
end;

function IsOdd(n: integer): integer; forward;

function IsEven(n: integer): integer;
begin
  if n = 0 then
    IsEven := 1
  else
    IsEven := IsOdd(n - 1);
end;

function IsOdd(n: integer): integer;
begin
  if n = 0 then
    IsOdd := 0
  else
    IsOdd := IsEven(n - 1);
end;

var
  i, parity: integer;
begin
  t0 := RealTimeClock();
  checkResult := Fib(27);
  parity := 0;
  for i := 1 to 40 do
    parity := parity + IsEven(2000 + i);
  checkResult := checkResult * 100 + parity;
  t1 := RealTimeClock();
  writeln('check=', checkResult);
  writeln('elapsed_s=', (t1 - t0):0:6);
end.
