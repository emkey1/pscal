program ChannelBasicSmoke;
{ VM 2.0 Phase 5b checkpoint 5b-i (Docs/pscal_vm2_plan.md Sec 6.2):
  single-threaded correctness smoke test for every Channel builtin --
  concurrency itself is covered separately by channel_spsc_backpressure.pas
  and channel_close_wakes_all.pas; this fixture is purely about each
  operation's return-value contract in isolation. }
var
  ch: channel;
  ok: boolean;
  got, tr, status, outVal: integer;
begin
  ok := true;

  { capacity-2 channel, same-thread create/send/receive round trip }
  ch := ChannelCreate(2);

  { TrySend into an empty channel should succeed immediately (1) }
  tr := ChannelTrySend(ch, 111);
  if tr <> 1 then begin writeln('FAIL: TrySend #1 expected 1 got ', tr); ok := false; end;

  { TrySend again -- buffer now has room for 1 more (capacity 2) }
  tr := ChannelTrySend(ch, 222);
  if tr <> 1 then begin writeln('FAIL: TrySend #2 expected 1 got ', tr); ok := false; end;

  { TrySend a third time should fail: buffer full (0) }
  tr := ChannelTrySend(ch, 333);
  if tr <> 0 then begin writeln('FAIL: TrySend #3 expected 0 (full) got ', tr); ok := false; end;

  { drain both buffered items via blocking Receive (won't actually block,
    the data is already there) }
  got := ChannelReceive(ch);
  if got <> 111 then begin writeln('FAIL: Receive #1 expected 111 got ', got); ok := false; end;
  got := ChannelReceive(ch);
  if got <> 222 then begin writeln('FAIL: Receive #2 expected 222 got ', got); ok := false; end;

  { TryReceive on now-empty-but-open channel: status=0 (would block), not -1 }
  ChannelTryReceive(ch, status, outVal);
  if status <> 0 then begin writeln('FAIL: TryReceive on empty-open expected status=0 got ', status); ok := false; end;

  { Send + Receive round trip through the blocking API too }
  ChannelSend(ch, 999);
  got := ChannelReceive(ch);
  if got <> 999 then begin writeln('FAIL: blocking Send/Receive expected 999 got ', got); ok := false; end;

  { IsClosed before Close }
  if ChannelIsClosed(ch) then begin writeln('FAIL: IsClosed true before Close'); ok := false; end;

  ChannelClose(ch);

  if not ChannelIsClosed(ch) then begin writeln('FAIL: IsClosed false after Close'); ok := false; end;

  { Closing twice is a no-op, not an error }
  ChannelClose(ch);
  if not ChannelIsClosed(ch) then begin writeln('FAIL: IsClosed false after double Close'); ok := false; end;

  { Receive on closed+drained: nil, not a hang or error }
  got := ChannelReceive(ch);
  if got <> nil then begin writeln('FAIL: Receive on closed+drained expected nil got ', got); ok := false; end;

  { TryReceive on closed+drained: status=-1 }
  ChannelTryReceive(ch, status, outVal);
  if status <> -1 then begin writeln('FAIL: TryReceive on closed+drained expected status=-1 got ', status); ok := false; end;

  { Send on a closed channel raises a runtime error -- tested separately
    in channel_send_after_close_errors.pas, since triggering it here would
    abort this program before it could report its own PASS/FAIL summary. }

  if ok then
    writeln('OK')
  else
    writeln('FAILED');
end.
