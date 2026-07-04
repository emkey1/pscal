program BenchGlobals;
{ Global-variable-heavy access: the loop body reads and writes eight
  program-level globals every iteration.  Exercises GET_GLOBAL/SET_GLOBAL
  (baseline for Phase 2b slot-addressed globals). }

const
  ITERS = 120000;

var
  t0, t1: double;
  g1, g2, g3, g4, g5, g6, g7, g8: integer;
  gCounter: integer;
  checkResult: integer;

procedure Step;
begin
  g1 := (g1 + g8) mod 999983;
  g2 := (g2 + g1) mod 999979;
  g3 := (g3 + g2) mod 1000003;
  g4 := (g4 + g3) mod 1000033;
  g5 := (g5 + g4) mod 1000037;
  g6 := (g6 + g5) mod 1000039;
  g7 := (g7 + g6) mod 1000081;
  g8 := (g8 + g7) mod 1000099;
  gCounter := gCounter + 1;
end;

var
  i: integer;
begin
  g1 := 1; g2 := 2; g3 := 3; g4 := 4;
  g5 := 5; g6 := 6; g7 := 7; g8 := 8;
  gCounter := 0;
  t0 := RealTimeClock();
  for i := 1 to ITERS do
    Step;
  checkResult := (g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + gCounter) mod 1000000007;
  t1 := RealTimeClock();
  writeln('check=', checkResult);
  writeln('elapsed_s=', (t1 - t0):0:6);
end.
