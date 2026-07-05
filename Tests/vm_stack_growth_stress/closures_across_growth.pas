program ClosuresAcrossGrowth;
{ VM 2.0 Phase 3, closures/upvalues investigation (see this phase's writeup
  in Docs/pscal_vm2_plan.md §5.9): an ESCAPING closure literal (`Next` is
  assigned out to `MakeGen`'s result, so the compiler's pre-existing
  closure_escapes analysis, compiler.c's closureLiteralEscapesCurrentRoutine,
  already promotes its captured `current` to a heap-owned cell rather than
  a raw stack Value*) must keep working when its creation is interleaved
  with deep recursion that repeatedly grows and re-shrinks the operand
  stack. This proves the append-only, never-relocated growth design does
  not disturb the existing escape-analysis/heap-promotion mechanism: each
  of 2000 independently-created generators, invoked only after ALL of them
  were created (long after their capturing frames returned and that stack
  region was reused many times over by DeepNoop's recursion), must report
  its own distinct, uncorrupted state. }

type
  TGen = function: integer;

function MakeGen(start: integer): TGen;
  var current: integer;
  function Next: integer;
  begin
    current := current + 1;
    Next := current;
  end;
begin
  current := start;
  MakeGen := @Next;
end;

function DeepNoop(n: integer): integer;
begin
  if n <= 0 then DeepNoop := 0
  else DeepNoop := DeepNoop(n - 1);
end;

var
  gens: array[1..2000] of TGen;
  g: TGen;
  i, dummy, ok, got: integer;
begin
  for i := 1 to 2000 do
  begin
    gens[i] := MakeGen(i * 10);
    { Force many stack growth/shrink cycles between each closure creation. }
    dummy := DeepNoop(3000);
  end;
  ok := 1;
  for i := 1 to 2000 do
  begin
    g := gens[i];
    got := g();
    if got <> (i * 10 + 1) then
    begin
      writeln('MISMATCH at ', i, ': got ', got);
      ok := 0;
    end;
  end;
  if ok = 1 then writeln('ALL 2000 CLOSURES OK')
  else writeln('FAILURE');
end.
