program ChannelSpscBackpressure;
{ VM 2.0 Phase 5b checkpoint 5b-i (Docs/pscal_vm2_plan.md Sec 6.2): a
  single producer (TaskSpawn'd) sends 500 items into a capacity-3 channel
  while the main thread consumes them concurrently. Capacity is
  deliberately much smaller than the item count so the producer's Send
  genuinely blocks on a full buffer and the consumer's Receive genuinely
  blocks on an empty one, exercising vmChannelSend/vmChannelReceive's
  actual wait-loop paths (pthread_cond_timedwait), not just their
  fast/never-blocks branches. Checksum-verified, matching this codebase's
  established "exact value check, no tolerance for a torn/wrong value"
  convention for concurrency fixtures (see task_concurrent_spawn_await.pas). }
var
  ch: channel;
  producerTask: task;

function Produce(c: channel; count: integer): integer;
var i: integer;
begin
  for i := 1 to count do
    ChannelSend(c, i);
  ChannelClose(c);
  Produce := count;
end;

var
  total, i, v, actualSum, expectedSum, producerResult: integer;
begin
  total := 500;
  ch := ChannelCreate(3);
  producerTask := TaskSpawn('Produce', ch, total);

  actualSum := 0;
  for i := 1 to total do
  begin
    v := ChannelReceive(ch);
    actualSum := actualSum + v;
  end;

  producerResult := TaskAwait(producerTask);
  expectedSum := (total * (total + 1)) div 2;

  if (actualSum = expectedSum) and (producerResult = total) and ChannelIsClosed(ch) then
    writeln('OK sum=', actualSum, ' producerResult=', producerResult)
  else
    writeln('FAILED actualSum=', actualSum, ' expected=', expectedSum,
             ' producerResult=', producerResult, ' closed=', ChannelIsClosed(ch));
end.
