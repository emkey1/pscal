program TaskConcurrentSpawnAwait;
{ VM 2.0 Phase 5a checkpoint 5a-i (Docs/pscal_vm2_plan.md Sec 6.1): stress
  test for TYPE_TASK's concurrent-use path. Four worker threads (spawned via
  the existing THREAD_CREATE-backed `spawn`/`join`) each independently run a
  loop of TaskSpawn -> TaskDone(poll) -> TaskAwait cycles. Each worker VM
  owns its OWN independent threads[] pool (createThreadJob indexes whichever
  VM* it's given directly, never resolving to a shared root -- confirmed by
  reading vm.c, not assumed), so this is not literally four threads
  contending for one shared array; what it does exercise is the same
  result-handoff mutex/cond machinery (the prerequisite lock-discipline fix,
  pscal-core 43b4077: statusReady/resultReady/awaitingReuse/active field
  protection) running under genuine wall-clock concurrency across four
  threads at once, plus shared process-global state every pool touches
  (the ObjHeader destructor registry, the heap allocator, the
  runtimeVTables/shell-builtin-profile caches the OTHER prerequisite fix in
  this same commit protects). Each Adder(a,b) task's result is checked
  exactly (no tolerance for a torn/wrong value -- unlike the benign-race
  globals fixture, task results must always be exactly right). }
var
  mismatches: integer;
  mid: integer;

function Adder(a, b: integer): integer;
begin
  Adder := a + b;
end;

procedure RunnerLoop(base: integer);
var i, t, expected, got: integer;
begin
  for i := 1 to 40 do
  begin
    t := TaskSpawn('Adder', base, i);
    expected := base + i;
    { poll TaskDone once purely to exercise it under concurrency; a task
      finishing this fast is not itself wrong, so its answer is unchecked }
    if TaskDone(t) then
      got := got;
    got := TaskAwait(t);
    if got <> expected then
    begin
      lock(mid);
      mismatches := mismatches + 1;
      unlock(mid);
      writeln('MISMATCH base=', base, ' i=', i, ' expected=', expected, ' got=', got);
    end;
  end;
end;

procedure Runner1; begin RunnerLoop(1000); end;
procedure Runner2; begin RunnerLoop(2000); end;
procedure Runner3; begin RunnerLoop(3000); end;
procedure Runner4; begin RunnerLoop(4000); end;

var
  w1, w2, w3, w4: integer;
begin
  mismatches := 0;
  mid := mutex();
  w1 := spawn Runner1;
  w2 := spawn Runner2;
  w3 := spawn Runner3;
  w4 := spawn Runner4;
  join w1; join w2; join w3; join w4;
  destroy(mid);
  if mismatches = 0 then
    writeln('OK mismatches=0')
  else
    writeln('FAILED mismatches=', mismatches);
end.
