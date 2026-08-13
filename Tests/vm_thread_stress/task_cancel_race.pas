program TaskCancelRace;
{ VM 2.0 Phase 5a checkpoint 5a-i (Docs/pscal_vm2_plan.md Sec 6.1): repeatedly
  spawn a task and immediately race TaskCancel against its natural
  completion (sometimes the cancel wins and the worker never really starts,
  sometimes the task finishes before the cancel is even observed -- both
  are legitimate outcomes, see vmBuiltinTaskCancel's comment). What this
  checks is that NEITHER outcome ever crashes, hangs, or corrupts the pool:
  every iteration must reach TaskAwait and get either a real value or a
  graceful nil, then the pool slot must be reusable for the next iteration
  (if slot reuse were broken, this loop would eventually exhaust
  VM_MAX_THREADS and TaskSpawn would start returning -1). }
var
  i, t, got: integer;

function Quick(n: integer): integer;
var j, acc: integer;
begin
  acc := 0;
  for j := 1 to 5000 do
    acc := acc + (j mod 3);
  Quick := acc + n;
end;

begin
  { sequential, not concurrent: each iteration TaskAwaits before the next
    TaskSpawn, so a working pool never needs more than 1 worker slot at a
    time here -- if slot reuse were broken, TaskSpawn would eventually
    exhaust VM_MAX_THREADS and either raise a "Task limit exceeded" runtime
    error (which fails this program loudly) or, worse, hang -- either is
    exactly what this fixture exists to catch. }
  for i := 1 to 300 do
  begin
    t := TaskSpawn('Quick', i);
    { cancel two iterations out of three -- still races completion often
      (spawn/schedule overhead usually beats 5000 loop iterations), but
      leaves a healthier mix of "let it run" cases than canceling every
      single time would. }
    if (i mod 3) <> 0 then
      TaskCancel(t);
    got := TaskAwait(t);
    { got is either Quick(i)'s real result or nil(=0); either is fine }
  end;
  writeln('OK iterations=300');
end.
