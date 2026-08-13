program ChannelSendAfterCloseErrors;
{ VM 2.0 Phase 5b checkpoint 5b-i (Docs/pscal_vm2_plan.md Sec 6.2): sending
  on a closed channel must raise a runtime error (matching Go's "send on
  closed channel panics" convention -- see ChannelSend's comment in
  builtin.c for why this is deliberately not a silently-tolerated no-op).
  Split into its own fixture, separate from channel_basic_smoke.pas, since
  the whole point is that this program is EXPECTED to abort. }
var
  ch: channel;
begin
  ch := ChannelCreate(1);
  ChannelClose(ch);
  ChannelSend(ch, 42); { expected to raise a runtime error and abort here }
  writeln('FAIL: ChannelSend on a closed channel did not raise an error');
end.
