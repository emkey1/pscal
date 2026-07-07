program MTStaticCoWRace;
{ VM 2.0 Phase 4j (Stage B): two threads racing a cheap retain-share copy
  (copyValueForStack, "localCopy := sharedStatic") against valueEnsureUnique's
  write-time clone-if-shared ("sharedStatic.x := i", via GET_FIELD_ADDRESS)
  on the SAME shared TYPE_RECORD. Before valueEnsureUnique() serialized on
  value_cell_mutex, two threads could each independently observe
  refcount > 1, each decide to clone, and then stomp on each other's
  install/release -- a confirmed real heap-corruption crash (macOS malloc
  zone integrity trap) within a handful of runs at 20k iterations/thread.
  This is a stop-ship-if-it-regresses test, not a hypothetical. }
const
  ITERATIONS = 50000;
type
  TStaticRec = record
    x: integer;
    y: integer;
  end;
var
  sharedStatic: TStaticRec;
  staticDone: array[1..4] of Boolean;

procedure StaticCopyAndMutate1;
var i: Integer; localCopy: TStaticRec;
begin
  for i := 1 to ITERATIONS do begin
    localCopy := sharedStatic;
    sharedStatic.x := i;
    sharedStatic.y := localCopy.x;
  end;
  staticDone[1] := true;
end;

procedure StaticCopyAndMutate2;
var i: Integer; localCopy: TStaticRec;
begin
  for i := 1 to ITERATIONS do begin
    localCopy := sharedStatic;
    sharedStatic.x := i;
    sharedStatic.y := localCopy.x;
  end;
  staticDone[2] := true;
end;

var
  h1, h2: Integer;
begin
  staticDone[1] := false;
  staticDone[2] := false;
  sharedStatic.x := 0;
  sharedStatic.y := 0;
  h1 := CreateThread(@StaticCopyAndMutate1);
  h2 := CreateThread(@StaticCopyAndMutate2);
  WaitForThread(h1);
  WaitForThread(h2);
  if staticDone[1] and staticDone[2] then
    writeln('PASS: static record race test completed cleanly')
  else
    writeln('FAIL');
end.
