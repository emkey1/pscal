program CreateThreadForwardProcAddress;

type
  TPayload = record
    inputVal: Integer;
    outputVal: Integer;
  end;

procedure Worker(payload: Pointer); forward;

procedure RunTest;
var
  payload: ^TPayload;
  t: Thread;
  status: Integer;
begin
  new(payload);
  payload^.inputVal := 33;
  payload^.outputVal := 0;

  t := CreateThread(@Worker, payload);
  status := WaitForThread(t);

  if status <> 0 then writeln('FAIL: wait status ', status);
  if payload^.outputVal <> 99 then writeln('FAIL: worker output ', payload^.outputVal);

  dispose(payload);
  writeln('PASS: CreateThread forward proc address');
end;

procedure Worker(payload: Pointer);
var
  data: ^TPayload;
begin
  data := payload;
  data^.outputVal := data^.inputVal * 3;
end;

begin
  RunTest;
end.
