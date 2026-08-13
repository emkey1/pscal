program GlobalsConcurrencyStress;
{ VM 2.0 Phase 2b (plan Docs/pscal_vm2_plan.md §5.7): stress test for the
  Value globals[] / chunk->global_slots[] migration's cross-thread locking
  discipline. Six worker threads run concurrently against TEN shared
  globals with NO mutex at all (g0..g9) plus one mutex-protected shared
  counter, all resolved via GET_GSLOT/SET_GSLOT. The unprotected globals
  intentionally tolerate the same benign races the pre-2b GET_GLOBAL fast
  path already tolerated (a torn read of an in-flight SET_GSLOT is not new
  behavior this phase introduces); what this test is actually checking is
  that chunk->global_slots[] itself -- the array all six threads index
  concurrently -- never produces a crash, an out-of-bounds access, or a
  used-after-free Symbol* under concurrent GET_GSLOT/SET_GSLOT traffic.
  Run under ASan+UBSan (build-asan) and repeated many times; a locking
  regression should show up as a sanitizer report or a hang, not a "wrong
  number" (this test does not assert an exact final value for g0..g9,
  only that the mutex-protected counter is exactly right and that nothing
  crashes/hangs). }
var
  counter: integer;
  mid: integer;
  g0, g1, g2, g3, g4, g5, g6, g7, g8, g9: integer;
  done0, done1, done2, done3, done4, done5: integer;

procedure Worker0;
var i, sum: integer;
begin
  for i := 1 to 2000 do
  begin
    lock(mid); counter := counter + 1; unlock(mid);
    g0 := g0 + 1;
    sum := g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + g9;
    g1 := sum mod 997;
  end;
  done0 := 1;
end;

procedure Worker1;
var i, sum: integer;
begin
  for i := 1 to 2000 do
  begin
    lock(mid); counter := counter + 1; unlock(mid);
    g2 := g2 + 1;
    sum := g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + g9;
    g3 := sum mod 997;
  end;
  done1 := 1;
end;

procedure Worker2;
var i, sum: integer;
begin
  for i := 1 to 2000 do
  begin
    lock(mid); counter := counter + 1; unlock(mid);
    g4 := g4 + 1;
    sum := g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + g9;
    g5 := sum mod 997;
  end;
  done2 := 1;
end;

procedure Worker3;
var i, sum: integer;
begin
  for i := 1 to 2000 do
  begin
    lock(mid); counter := counter + 1; unlock(mid);
    g6 := g6 + 1;
    sum := g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + g9;
    g7 := sum mod 997;
  end;
  done3 := 1;
end;

procedure Worker4;
var i, sum: integer;
begin
  for i := 1 to 2000 do
  begin
    lock(mid); counter := counter + 1; unlock(mid);
    g8 := g8 + 1;
    sum := g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + g9;
    g9 := sum mod 997;
  end;
  done4 := 1;
end;

procedure Worker5;
var i: integer;
begin
  { pure reader: no writes of its own, just hammers the shared slots'
    read path (GET_GSLOT) concurrently with the other five threads'
    GET_GSLOT/SET_GSLOT traffic on the very same slots. }
  for i := 1 to 2000 do
  begin
    lock(mid); counter := counter + 0; unlock(mid);
    if (g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 + g8 + g9) < -1000000000 then
      writeln('unreachable');
  end;
  done5 := 1;
end;

var
  t0, t1, t2, t3, t4, t5: integer;
begin
  counter := 0;
  g0 := 0; g1 := 0; g2 := 0; g3 := 0; g4 := 0;
  g5 := 0; g6 := 0; g7 := 0; g8 := 0; g9 := 0;
  done0 := 0; done1 := 0; done2 := 0; done3 := 0; done4 := 0; done5 := 0;
  mid := mutex();
  t0 := spawn Worker0;
  t1 := spawn Worker1;
  t2 := spawn Worker2;
  t3 := spawn Worker3;
  t4 := spawn Worker4;
  t5 := spawn Worker5;
  join t0; join t1; join t2; join t3; join t4; join t5;
  destroy(mid);
  { Workers 0-4 each add 1 to counter per iteration (2000 iterations);
    Worker5 is a pure reader that adds 0 -- see its comment above. }
  if counter = 10000 then
    writeln('OK counter=', counter)
  else
    writeln('MISMATCH counter=', counter);
end.
