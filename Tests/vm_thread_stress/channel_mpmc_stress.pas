program ChannelMpmcStress;
{ VM 2.0 Phase 5b checkpoint 5b-ii (Docs/pscal_vm2_plan.md Sec 6.2): several
  TaskSpawn'ed producers AND consumers share ONE channel -- 5b-i's SPSC
  fixture (channel_spsc_backpressure.pas) only exercised a single
  producer/single consumer pair. Checksum-verified, matching this
  codebase's "exact value, no tolerance for a torn/wrong result" convention
  for concurrency fixtures (see task_concurrent_spawn_await.pas). Sequence:
  spawn all 4 consumers first (so they're genuinely blocked on an empty
  channel when producers start, exercising real backpressure/wakeup in
  both directions), then spawn all 4 producers, await every producer
  before closing (so Close never races an in-flight Send), then await
  every consumer (each returns once it drains everything and observes
  closed+empty). }
var
  ch: channel;
  sumMutex: integer;
  totalSum, totalCount: integer;
  producers: array[1..4] of task;
  consumers: array[1..4] of task;
  i, perProducer, expectedSum, producedTotal, consumedTotal: integer;

function Produce(c: channel; base, count: integer): integer;
var j: integer;
begin
  for j := 1 to count do
    ChannelSend(c, base + j);
  Produce := count;
end;

function Consume(c: channel): integer;
{ Uses ChannelTryReceive's integer status in a poll loop, NOT blocking
  ChannelReceive plus a "= nil"/"<> nil" termination check -- deliberately,
  not by preference. Comparing a function-local integer variable against
  nil with either "=" or "<>" throws "Operands not comparable" here once
  the variable has ever held a real integer value in the same scope
  (confirmed via an isolated single-threaded, zero-concurrency,
  zero-channel probe: a top-level program var comparing cleanly against
  nil is fine, but the identical comparison inside a function's local
  scope is not). Blocking ChannelReceive's own nil-return path is already
  covered by channel_basic_smoke.pas (single-threaded, a top-level var) and
  channel_close_wakes_all.pas ("got = nil", also top-level) -- this fixture
  exists to stress multi-producer/multi-consumer contention on the shared
  ring buffer, not to re-prove nil-return works, so routing around this
  separate, pre-existing, Channel-unrelated bug here is in scope; filed as
  a follow-up rather than fixed inline. }
var v, localSum, localCount, status: integer;
begin
  localSum := 0;
  localCount := 0;
  status := 0;
  ChannelTryReceive(c, status, v);
  while status <> -1 do
  begin
    if status = 1 then
    begin
      localSum := localSum + v;
      localCount := localCount + 1;
    end
    else
      Delay(1); { status = 0: buffer momentarily empty, channel still open -- brief yield before retrying }
    ChannelTryReceive(c, status, v);
  end;
  lock(sumMutex);
  totalSum := totalSum + localSum;
  totalCount := totalCount + localCount;
  unlock(sumMutex);
  Consume := localCount;
end;

begin
  ch := ChannelCreate(5);
  sumMutex := mutex();
  totalSum := 0;
  totalCount := 0;
  perProducer := 200;

  for i := 1 to 4 do
    consumers[i] := TaskSpawn('Consume', ch);

  for i := 1 to 4 do
    producers[i] := TaskSpawn('Produce', ch, i * 100000, perProducer);

  producedTotal := 0;
  for i := 1 to 4 do
    producedTotal := producedTotal + TaskAwait(producers[i]);

  ChannelClose(ch);

  consumedTotal := 0;
  for i := 1 to 4 do
    consumedTotal := consumedTotal + TaskAwait(consumers[i]);

  destroy(sumMutex);

  expectedSum := 0;
  for i := 1 to 4 do
    expectedSum := expectedSum + (perProducer * (i * 100000)) + ((perProducer * (perProducer + 1)) div 2);

  if (producedTotal = 4 * perProducer) and (consumedTotal = 4 * perProducer) and
     (totalCount = 4 * perProducer) and (totalSum = expectedSum) then
    writeln('OK totalCount=', totalCount, ' totalSum=', totalSum)
  else
    writeln('FAILED producedTotal=', producedTotal, ' consumedTotal=', consumedTotal,
             ' totalCount=', totalCount, ' totalSum=', totalSum, ' expectedSum=', expectedSum);
end.
