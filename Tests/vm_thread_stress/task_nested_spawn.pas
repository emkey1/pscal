program TaskNestedSpawn;
{ VM 2.0 Phase 5a checkpoint 5a-i (Docs/pscal_vm2_plan.md Sec 6.1): a task
  whose own work function itself calls TaskSpawn/TaskAwait, exercising
  createThreadJob's nested-pool behavior (each spawned worker VM owns its
  own independent threads[] pool, so a child TaskSpawn issued from inside
  a parent task's worker thread must correctly build and tear down a
  SECOND, independent pool on that worker's own VM instance, not corrupt
  or contend with the parent VM's pool). Two levels deep: main spawns
  Outer, Outer spawns several Inner tasks and sums their results. }
var
  final: integer;

function Inner(n: integer): integer;
begin
  Inner := n * n;
end;

function Outer(count: integer): integer;
var i, t, sum: integer;
  tasks: array[1..8] of integer;
begin
  sum := 0;
  for i := 1 to count do
    tasks[i] := TaskSpawn('Inner', i);
  for i := 1 to count do
    sum := sum + TaskAwait(tasks[i]);
  Outer := sum;
end;

var
  t: integer;
  expected: integer;
  i: integer;
begin
  expected := 0;
  for i := 1 to 8 do
    expected := expected + i * i;

  t := TaskSpawn('Outer', 8);
  final := TaskAwait(t);

  if final = expected then
    writeln('OK final=', final)
  else
    writeln('MISMATCH expected=', expected, ' got=', final);
end.
