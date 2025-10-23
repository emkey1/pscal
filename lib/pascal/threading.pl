unit Threading;

interface

type
  ThreadRequestOptions = record
    Name: string;
    SubmitOnly: Boolean;
  end;

function ThreadOptionsNamed(const ThreadName: string): ThreadRequestOptions;
function ThreadOptionsQueue(const ThreadName: string): ThreadRequestOptions;
function ThreadOptionsQueueOnly: ThreadRequestOptions;

function SpawnBuiltinNamed(const BuiltinName, ThreadName: string): Integer;
function PoolSubmitNamed(const BuiltinName, ThreadName: string): Integer;
function SpawnBuiltinWithOptions(const BuiltinName: string; const Options: ThreadRequestOptions): Integer;
function PoolSubmitWithOptions(const BuiltinName: string; const Options: ThreadRequestOptions): Integer;

function LookupThreadByName(const ThreadName: string): Integer;
function PauseThreadHandle(ThreadId: Integer): Boolean;
function ResumeThreadHandle(ThreadId: Integer): Boolean;
function CancelThreadHandle(ThreadId: Integer): Boolean;
function ThreadStatusOk(ThreadId: Integer; DropResult: Boolean): Boolean;
function ThreadStatsCount: Integer;

implementation

function ThreadOptionsNamed(const ThreadName: string): ThreadRequestOptions;
begin
  ThreadOptionsNamed.Name := ThreadName;
  ThreadOptionsNamed.SubmitOnly := False;
end;

function ThreadOptionsQueue(const ThreadName: string): ThreadRequestOptions;
begin
  ThreadOptionsQueue.Name := ThreadName;
  ThreadOptionsQueue.SubmitOnly := True;
end;

function ThreadOptionsQueueOnly: ThreadRequestOptions;
begin
  ThreadOptionsQueueOnly.Name := '';
  ThreadOptionsQueueOnly.SubmitOnly := True;
end;

function SpawnBuiltinNamed(const BuiltinName, ThreadName: string): Integer;
begin
  SpawnBuiltinNamed := ThreadSpawnBuiltin(BuiltinName, ThreadOptionsNamed(ThreadName));
end;

function PoolSubmitNamed(const BuiltinName, ThreadName: string): Integer;
begin
  PoolSubmitNamed := ThreadPoolSubmit(BuiltinName, ThreadOptionsQueue(ThreadName));
end;

function SpawnBuiltinWithOptions(const BuiltinName: string; const Options: ThreadRequestOptions): Integer;
begin
  SpawnBuiltinWithOptions := ThreadSpawnBuiltin(BuiltinName, Options);
end;

function PoolSubmitWithOptions(const BuiltinName: string; const Options: ThreadRequestOptions): Integer;
begin
  PoolSubmitWithOptions := ThreadPoolSubmit(BuiltinName, Options);
end;

function LookupThreadByName(const ThreadName: string): Integer;
begin
  LookupThreadByName := ThreadLookup(ThreadName);
end;

function PauseThreadHandle(ThreadId: Integer): Boolean;
begin
  PauseThreadHandle := ThreadPause(ThreadId) <> 0;
end;

function ResumeThreadHandle(ThreadId: Integer): Boolean;
begin
  ResumeThreadHandle := ThreadResume(ThreadId) <> 0;
end;

function CancelThreadHandle(ThreadId: Integer): Boolean;
begin
  CancelThreadHandle := ThreadCancel(ThreadId) <> 0;
end;

function ThreadStatusOk(ThreadId: Integer; DropResult: Boolean): Boolean;
begin
  ThreadStatusOk := ThreadGetStatus(ThreadId, DropResult) <> 0;
end;

function ThreadStatsCount: Integer;
begin
  ThreadStatsCount := length(ThreadStats());
end;

end.
