program ReplayHttpFile;
{ VM 2.0 Phase 6 record/replay adversarial/functional test
  (Docs/pscal_vm2_plan.md Sec 6.3): writes/reads a scratch file (plain file
  I/O, readln's VAR string writeback) and fetches a *separate*,
  externally-provided file via an HTTP file:// loopback request
  (HttpRequest's VAR mstream writeback). The HTTP target is intentionally
  not touched by this program itself, so a driver can mutate it between a
  --fx-record run and a --fx-replay run to prove the replay used the
  journaled response rather than re-fetching live. }
var
  dir, path, url, httpTargetPath: string;
  f: text;
  line: string;
  fileAcc, httpAcc: integer;
  s, code: integer;
  ms: MStream;
begin
  dir := GetEnv('VM_FX_TEST_TMP');
  if dir = '' then dir := '.';
  path := dir + '/vm_fx_replay_scratch.txt';

  assign(f, path);
  rewrite(f);
  writeln(f, 'alpha payload one');
  writeln(f, 'beta payload two');
  writeln(f, 'gamma payload three');
  close(f);

  fileAcc := 0;
  assign(f, path);
  reset(f);
  while not eof(f) do
  begin
    readln(f, line);
    fileAcc := fileAcc + length(line);
  end;
  close(f);

  httpTargetPath := GetEnv('VM_FX_HTTP_TARGET');
  if httpTargetPath = '' then httpTargetPath := path;
  if copy(httpTargetPath, 1, 1) = '/' then
    url := 'file://' + httpTargetPath
  else
    url := 'file://' + GetEnv('PWD') + '/' + httpTargetPath;

  ms := mstreamcreate();
  s := HttpSession();
  code := HttpRequest(s, 'GET', url, nil, ms);
  httpAcc := code * 100000 + length(MStreamBuffer(ms));
  mstreamfree(ms);
  HttpClose(s);

  writeln('check=', fileAcc, ',', httpAcc);
end.
