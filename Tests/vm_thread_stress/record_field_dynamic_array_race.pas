program RecordFieldDynamicArrayRace;
{ VM 2.0 follow-up to dynamic_array_fresh_publish_race (pscal-core f65432e,
  Tests/vm_thread_stress/dynamic_array_setlength_race.pas). f65432e fixed
  LOAD_ELEMENT_VALUE/_CONST and resolveFirstDimBounds's "peek
  ARRAY_IS_DYNAMIC(*candidate) before any lock" shape for plain global
  dynamic arrays, but vm.c's pushFieldValueByName (used by
  LOAD_FIELD_VALUE_BY_NAME/16, e.g. reading `rec.dynField` or
  `Length(rec.dynField)`) had the exact same shape for a record FIELD's
  dynamic-array value: it checked ARRAY_IS_DYNAMIC(*fieldStorage) before
  ever taking a lock, which itself dereferences the field's live,
  concurrently-replaceable ArrayObj pointer -- racing a concurrent
  SetLength(rec.dynField, ...) on another thread exactly like the
  already-fixed plain-global case.

  Fixed by making pushFieldValueByName call copyDynamicArraySnapshotValue()
  unconditionally first and branch on dynamic-vs-static from the resulting
  safe local copy, matching LOAD_ELEMENT_VALUE's own fix.

  This is a stop-ship-if-it-regresses test, not a hypothetical. }
const
  ITERATIONS = 20000;
type
  RecT = record
    dynField: array of integer;
  end;
var
  sharedRec: RecT;
  readerDone, writerDone: Boolean;

procedure FieldReader;
var i, n, v: Integer;
begin
  for i := 1 to ITERATIONS do begin
    n := Length(sharedRec.dynField);
    if n > 0 then v := sharedRec.dynField[0];
  end;
  readerDone := true;
end;

procedure FieldWriter;
var i: Integer;
begin
  for i := 1 to ITERATIONS do begin
    SetLength(sharedRec.dynField, (i mod 5) + 1);
    sharedRec.dynField[0] := i;
  end;
  writerDone := true;
end;

var
  hReader, hWriter: Integer;
begin
  readerDone := false;
  writerDone := false;
  SetLength(sharedRec.dynField, 1);
  sharedRec.dynField[0] := 0;
  hReader := CreateThread(@FieldReader);
  hWriter := CreateThread(@FieldWriter);
  WaitForThread(hReader);
  WaitForThread(hWriter);
  if readerDone and writerDone then
    writeln('PASS: record field dynamic array race test completed cleanly')
  else
    writeln('FAIL');
end.
