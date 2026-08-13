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
{ Blocking ChannelReceive plus a "<> nil" loop-termination check -- the
  natural idiom for draining a channel until it's closed and empty. An
  earlier version of this fixture routed around a real VM bug via
  ChannelTryReceive's integer status codes instead: comparing an ordinary
  ordinal value (e.g. a ChannelReceive result held in an `integer` var)
  against `nil` with "=" or "<>" unconditionally threw "Operands not
  comparable", because the comparison-opcode dispatcher in vm.c had no
  branch for "ordinal vs NIL" -- only pointer/interface/closure/nil-vs-nil
  were handled. Fixed in vm.c's EQUAL/NOT_EQUAL/etc. dispatcher: any
  concrete non-nil value now compares as simply "not nil" against a nil
  literal, matching ChannelReceive/TaskAwait's documented nil-return
  convention. Verified fixed here under real MPMC contention (5/5 clean
  runs) after the fix landed. }
var v, localSum, localCount: integer;
begin
  localSum := 0;
  localCount := 0;
  v := ChannelReceive(c);
  while v <> nil do
  begin
    localSum := localSum + v;
    localCount := localCount + 1;
    v := ChannelReceive(c);
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
