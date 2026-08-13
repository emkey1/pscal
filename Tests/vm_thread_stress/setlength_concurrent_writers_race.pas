program SetlengthConcurrentWritersRace;
{ VM 2.0 follow-up to dynamic_array_fresh_publish_race (pscal-core f65432e,
  Tests/vm_thread_stress/dynamic_array_setlength_race.pas). f65432e locked
  resizeDynamicArrayValue's (backend_ast/builtin.c) release-old/publish-new
  step against a concurrent READER, but resizeDynamicArrayValue's own
  initial, unlocked read of *array_value's current ArrayObj (used
  throughout the rest of that function to read the array's existing
  bounds/data for the resize) was still exposed to a SECOND thread
  concurrently calling SetLength() on the SAME array: if that other
  thread's own (now-locked) publish step ran while this thread was still
  mid-read, the ArrayObj this thread captured at entry could be freed out
  from under it -- a genuine use-after-free under concurrent-WRITER stress,
  distinct from the reader/writer case f65432e covered (no reader thread
  needed to trigger this one).

  Fixed by having resizeDynamicArrayValue take its own retained reference
  on the array's current ArrayObj (dynamic_array_refcount_mutex-protected,
  matching copyDynamicArraySnapshotValue's own convention) right at entry,
  independent of -- and released independently of -- whatever the live
  cell holds by the time this call's own publish step runs.

  This is a stop-ship-if-it-regresses test, not a hypothetical. }
const
  ITERATIONS = 20000;
var
  sharedDyn: array of integer;
  writer1Done, writer2Done: Boolean;

procedure Writer1;
var i: Integer;
begin
  for i := 1 to ITERATIONS do begin
    SetLength(sharedDyn, (i mod 5) + 1);
  end;
  writer1Done := true;
end;

procedure Writer2;
var i: Integer;
begin
  for i := 1 to ITERATIONS do begin
    SetLength(sharedDyn, (i mod 7) + 1);
  end;
  writer2Done := true;
end;

var
  hW1, hW2: Integer;
begin
  writer1Done := false;
  writer2Done := false;
  SetLength(sharedDyn, 1);
  sharedDyn[0] := 0;
  hW1 := CreateThread(@Writer1);
  hW2 := CreateThread(@Writer2);
  WaitForThread(hW1);
  WaitForThread(hW2);
  if writer1Done and writer2Done then
    writeln('PASS: setlength concurrent writers race test completed cleanly')
  else
    writeln('FAIL');
end.
