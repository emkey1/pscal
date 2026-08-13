program BenchDeepRecursion;
{ VM 2.0 Phase 3 (plan Docs/pscal_vm2_plan.md §5.9): recursion depth this
  benchmark exists to move. Pre-Phase-3, the fixed VM_CALL_STACK_MAX=4096 /
  VM_STACK_MAX=8192 arrays made a countdown this deep impossible (a clean
  "Call stack overflow"/"Stack overflow" at a few thousand frames); this
  benchmark exists to (a) prove the growable stack still succeeds at a
  depth that used to be unreachable, and (b) give the phase's own
  point -- recursion depth / footprint, not raw speed -- a tracked number
  alongside calls.p's existing CALL/RETURN throughput measurement. }

var
  t0, t1: double;
  checkResult: integer;

function countdown(n: integer): integer;
begin
  if n <= 0 then
    countdown := 0
  else
    countdown := 1 + countdown(n - 1);
end;

var
  i, total: integer;
begin
  t0 := RealTimeClock();
  total := 0;
  for i := 1 to 20 do
    total := total + countdown(50000);
  checkResult := total;
  t1 := RealTimeClock();
  writeln('check=', checkResult);
  writeln('elapsed_s=', (t1 - t0):0:6);
end.
