program ChannelCloseWakesAll;
{ VM 2.0 Phase 5b checkpoint 5b-i (Docs/pscal_vm2_plan.md Sec 6.2): N
  consumer tasks all block on ChannelReceive against the same empty
  channel; a single ChannelClose from the main thread must wake EVERY one
  of them (pthread_cond_broadcast, not signal) -- a broadcast/signal mixup
  here would leave N-1 tasks hanging forever, which this fixture would
  catch as a hang/timeout rather than a wrong-value assertion. }
var
  ch: channel;
  n: integer;
  tasks: array[1..8] of task;
  i, woke, got: integer;

function Consume(c: channel): integer;
begin
  Consume := ChannelReceive(c); { blocks until Close wakes it, returns nil }
end;

begin
  n := 8;
  ch := ChannelCreate(1);

  for i := 1 to n do
    tasks[i] := TaskSpawn('Consume', ch);

  { give every consumer a moment to actually reach its blocking wait
    before closing -- not required for correctness (Close is safe to call
    at any point), but makes the "were they actually blocked" intent of
    this fixture more meaningful }
  Delay(200);

  ChannelClose(ch);

  woke := 0;
  for i := 1 to n do
  begin
    got := TaskAwait(tasks[i]);
    if got = nil then
      woke := woke + 1;
  end;

  if woke = n then
    writeln('OK woke=', woke)
  else
    writeln('FAILED woke=', woke, ' expected=', n);
end.
