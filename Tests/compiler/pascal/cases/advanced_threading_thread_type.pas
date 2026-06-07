program Test_Advanced_Threading;

type
  TArgPayload = record
    inputVal: Integer;
    outputResult: Integer;
  end;

procedure AdvancedWorker(payload: Pointer);
var
  data: ^TArgPayload;
begin
  data := payload;
  data^.outputResult := data^.inputVal * 3;
end;

procedure RunTests;
var
  payload: ^TArgPayload;
  tHandle: Thread;
  waitStatus: Integer;
begin
  new(payload);
  payload^.inputVal := 33;
  payload^.outputResult := 0;

  tHandle := CreateThread(@AdvancedWorker, payload);
  waitStatus := WaitForThread(tHandle);

  if waitStatus <> 0 then
    writeln('FAIL: Thread manager framework returned unsuccessful execution code: ', waitStatus);

  if payload^.outputResult <> 99 then
    writeln('FAIL: Input state context parameter mapping failed to alter structure values under pointer routing');

  dispose(payload);
  writeln('SUITE 12 COMPLETE: Advanced Threading');
end;

begin
  RunTests;
end.
