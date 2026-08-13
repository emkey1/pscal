program DynamicArraySetlengthRace;
{ vmBuiltinSetlength's in-place array-resize logic (backend_ast/builtin.c's
  resizeDynamicArrayValue) had ZERO locking of any kind, confirmed present
  unchanged since before the entire VM 2.0 Value re-representation project
  (git show a940b43~1:src/backend_ast/builtin.c). One thread calling
  SetLength() on a SHARED GLOBAL dynamic array while another thread
  concurrently reads it (Length(), or an indexed element read) corrupted
  memory 100% reproducibly within 1-3 runs: SIGTRAP heap corruption, a
  double-release ("pscalObjRelease: double-release detected"), or a
  "garbage bounds" Array-index-out-of-bounds error.

  Root cause: resizeDynamicArrayValue publishes a freshly-built ArrayObj
  into the array's Value cell (releasing the old one) exactly like
  replaceValueCell's whole-value reassignment does -- but unlike
  replaceValueCell, it never took the matching value_cell_mutex +
  dynamic_array_refcount_mutex pair that copyDynamicArraySnapshotValue's
  read side needs (the dynamic_array_fresh_publish_race fix, pscal-core
  dbac64a, only covered whole-value reassignment, not SetLength's in-place
  resize -- a completely different code path). A second, narrower bug sat
  right next to it: several read sites (Length()'s resolveFirstDimBounds,
  and vm.c's LOAD_ELEMENT_VALUE/LOAD_ELEMENT_VALUE_CONST) checked
  ARRAY_IS_DYNAMIC(*candidate) *before* taking any lock -- that peek
  itself dereferences the array's live, concurrently-replaceable ArrayObj
  pointer, so it raced the exact same publish step even after SetLength's
  own locking was added. Both are fixed: resizeDynamicArrayValue now locks
  around its release-old/publish-new step (same order as replaceValueCell:
  value_cell_mutex, then dynamic_array_refcount_mutex), and every read site
  now calls copyDynamicArraySnapshotValue() unconditionally first and only
  branches on dynamic-vs-static from the resulting safe local copy.
  This is a stop-ship-if-it-regresses test, not a hypothetical. }
const
  ITERATIONS = 20000;
var
  sharedDyn: array of integer;
  readerDone, writerDone: Boolean;

procedure DynReader;
var i, n, v: Integer;
begin
  for i := 1 to ITERATIONS do begin
    n := Length(sharedDyn);
    if n > 0 then v := sharedDyn[0];
  end;
  readerDone := true;
end;

procedure DynWriter;
var i: Integer;
begin
  for i := 1 to ITERATIONS do begin
    SetLength(sharedDyn, (i mod 5) + 1);
    sharedDyn[0] := i;
  end;
  writerDone := true;
end;

var
  hReader, hWriter: Integer;
begin
  readerDone := false;
  writerDone := false;
  SetLength(sharedDyn, 1);
  sharedDyn[0] := 0;
  hReader := CreateThread(@DynReader);
  hWriter := CreateThread(@DynWriter);
  WaitForThread(hReader);
  WaitForThread(hWriter);
  if readerDone and writerDone then
    writeln('PASS: dynamic array race test completed cleanly')
  else
    writeln('FAIL');
end.
