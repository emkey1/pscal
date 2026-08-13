program HttpAsyncTaskStress;
{ VM 2.0 Phase 5a checkpoint 5a-iii (Docs/pscal_vm2_plan.md Sec 6.1): HTTP
  async now rides vmTaskCreateNative/TYPE_TASK instead of the old 32-slot
  g_http_async[] pool, and native-task cancellation no longer uses a
  separate Thread.nativeCancelFn hook (removed after a confirmed race --
  see vm.c's vmThreadCancel comment) -- work_fn now polls
  Thread.cancelRequested directly. This fixture stresses exactly that path:
  several concurrent file:// async transfers in flight at once, a mix of
  "let it complete" and "cancel mid-flight", repeated over many rounds, to
  catch use-after-free/leak/race regressions in httpAsyncWork's cleanup
  (httpAsyncJobFree via vmTaskCreateNative's cleanup contract) and in the
  growable thread-pool slot reuse under concurrent native-task churn. }
var
  workDir, srcPath: string;
  f: text;
  round, i, total: integer;
  ids: array[1..8] of task;
  outs: array[1..8] of mstream;
  sessions: array[1..8] of integer;
  code: integer;

function ResolveTempDir: string;
var candidate: string;
begin
  candidate := GetEnv('TMPDIR');
  candidate := candidate + '';
  if candidate = '' then candidate := '/tmp';
  ResolveTempDir := candidate;
end;

begin
  workDir := ResolveTempDir;
  if (Length(workDir) > 0) and (workDir[Length(workDir)] <> '/') then
    workDir := workDir + '/';
  srcPath := workDir + 'http_async_task_stress_src.txt';

  assign(f, srcPath);
  rewrite(f);
  for i := 1 to 20000 do
    writeln(f, 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx');
  close(f);

  for i := 1 to 8 do
    sessions[i] := HttpSession();

  total := 0;
  for round := 1 to 15 do
  begin
    { spawn 8 concurrent file:// async requests, each on its own reused
      session (sessions are configuration-only handles cheap to reuse
      across rounds; MAX_HTTP_SESSIONS is a fixed 32-slot pool, so creating
      a fresh session every round -- 8*15=120 -- would exhaust it). }
    for i := 1 to 8 do
      ids[i] := HttpRequestAsync(sessions[i], 'GET', 'file://' + srcPath, nil);

    { cancel half of them immediately (races completion), let the rest run }
    for i := 1 to 8 do
    begin
      if (i mod 2) = 0 then
        HttpCancel(ids[i]);
    end;

    for i := 1 to 8 do
    begin
      outs[i] := mstreamcreate();
      code := HttpAwait(ids[i], outs[i]);
      { code is either 200 (completed) or -1 (canceled); either is fine --
        what matters is every await returns promptly with no hang/crash }
      if (code <> 200) and (code <> -1) then
        writeln('UNEXPECTED code=', code, ' round=', round, ' i=', i);
      mstreamfree(outs[i]);
      total := total + 1;
    end;
  end;

  for i := 1 to 8 do
    HttpClose(sessions[i]);

  if FileExists(srcPath) then
  begin
    assign(f, srcPath);
    erase(f);
  end;

  writeln('OK completed=', total);
end.
