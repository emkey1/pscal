program GetElementAddressRace;
{ VM 2.0 follow-up to dynamic_array_fresh_publish_race (pscal-core f65432e,
  Tests/vm_thread_stress/dynamic_array_setlength_race.pas). That fix
  serialized resizeDynamicArrayValue's release-old/publish-new step against
  a concurrent READER's copyDynamicArraySnapshotValue(), but left
  GET_ELEMENT_ADDRESS/GET_ELEMENT_ADDRESS_CONST (vm.c) untouched:
  `sharedDyn[0] := i`-style element assignment compiles to
  GET_ELEMENT_ADDRESS + SET_INDIRECT, and GET_ELEMENT_ADDRESS must return a
  genuinely live address into the array's own storage (SET_INDIRECT writes
  through it) rather than a detached read-only copy. For a dynamic array it
  therefore aliased the live, concurrently-replaceable ArrayObj's buffer
  with zero protection -- a concurrent SetLength() resizing the same array
  could free that buffer while another thread's SET_INDIRECT was still
  writing through the address GET_ELEMENT_ADDRESS handed it.

  Fixed by having GET_ELEMENT_ADDRESS/_CONST take their own retained
  snapshot of the array (copyDynamicArraySnapshotValue(), same pattern
  f65432e already uses for the read opcodes) and transferring that retained
  ArrayObj reference into the returned pointer's PointerObj.retained_array
  field (core/types.h) instead of releasing it before the opcode returns --
  the buffer then stays alive for exactly as long as the pointer used to
  address into it does, even across a concurrent SetLength() on the live
  global it was taken from. freeValue()/makeCopyOfValue() release/re-retain
  it exactly like ArrayObj's own view_of mechanism does for jagged
  sub-array slices.

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
    sharedDyn[0] := i;
  end;
  writer1Done := true;
end;

procedure Writer2;
var i: Integer;
begin
  for i := 1 to ITERATIONS do begin
    SetLength(sharedDyn, (i mod 5) + 1);
    sharedDyn[0] := i;
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
    writeln('PASS: get_element_address race test completed cleanly')
  else
    writeln('FAIL');
end.
