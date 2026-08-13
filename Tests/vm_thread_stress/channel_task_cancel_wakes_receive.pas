program ChannelTaskCancelWakesReceive;
{ VM 2.0 Phase 5b checkpoint 5b-ii (Docs/pscal_vm2_plan.md Sec 6.2): a task
  blocked mid-ChannelReceive against a channel that will never receive
  anything must actually wake when TaskCancel'd, not hang forever --
  vmChannelOperationInterrupted polls Thread.cancelRequested via
  VM.owningThread on every 100ms wait-loop wake, the same poll-don't-
  callback pattern 5a-iii's native HTTP tasks use for cancellation. This
  fixture repeats the spawn/delay/cancel/await cycle many times (matching
  task_cancel_race.pas's own repetition strategy) to catch a rare race, not
  just rely on a single lucky pass.

  Note: "got" is compared against nil in the MAIN program body (top-level
  scope), not inside a function -- deliberately. A function-local integer
  variable compared against nil throws a runtime error in this VM
  (confirmed via an isolated probe, filed as a follow-up, see
  channel_mpmc_stress.pas's Consume() comment for the same finding); a
  top-level program variable does not, which is why BlockedReceive's own
  return value is checked here in the main body instead of inside a
  helper function. }
var
  i, iterations: integer;
  ch: channel;
  t: task;
  got: integer;

function BlockedReceive(c: channel): integer;
begin
  BlockedReceive := ChannelReceive(c); { never sent to -- blocks until canceled }
end;

begin
  iterations := 50;
  for i := 1 to iterations do
  begin
    ch := ChannelCreate(1); { fresh channel each iteration, never sent to }
    t := TaskSpawn('BlockedReceive', ch);
    Delay(150); { let the task genuinely reach its blocking wait -- the
                   wait loop's own 100ms poll granularity means this must
                   exceed 100ms for real confidence it's actually inside
                   pthread_cond_timedwait, not still starting up }
    TaskCancel(t);
    got := TaskAwait(t); { must return promptly -- if this hangs, the
                            whole program hangs and the outer timeout
                            catches it as a failure }
    if got = nil then
    begin
      { expected: canceled before anything was ever sent }
    end
    else
    begin
      writeln('UNEXPECTED: got a real value instead of nil at i=', i, ' got=', got);
    end;
  end;
  writeln('OK iterations=', iterations);
end.
