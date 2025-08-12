unit Dos;

interface

type
  SearchRec = record
    Name: string;
    Attr: integer;
  end;

function FindFirst(Path: string): string; (* returns first entry name, empty when none *)
function FindNext: string; (* returns next entry name *)
function GetFAttr(Path: string): integer;
procedure MkDir(Path: string);
procedure RmDir(Path: string);
function GetEnv(VarName: string): string;
procedure GetDate(var Year, Month, Day, Dow: word);
procedure GetTime(var Hour, Minute, Second, Sec100: word);
function Exec(Path, Cmd: string): integer;

implementation

function FindFirst(Path: string): string;
begin
  FindFirst := dos_findfirst(Path);
end;

function FindNext: string;
begin
  FindNext := dos_findnext();
end;

function GetFAttr(Path: string): integer;
begin
  GetFAttr := dos_getfattr(Path);
end;

procedure MkDir(Path: string);
begin
  dos_mkdir(Path);
end;

procedure RmDir(Path: string);
begin
  dos_rmdir(Path);
end;

function GetEnv(VarName: string): string;
begin
  GetEnv := dos_getenv(VarName);
end;

procedure GetDate(var Year, Month, Day, Dow: word);
begin
  dos_getdate(Year, Month, Day, Dow);
end;

procedure GetTime(var Hour, Minute, Second, Sec100: word);
begin
  dos_gettime(Hour, Minute, Second, Sec100);
end;

function Exec(Path, Cmd: string): integer;
begin
  Exec := dos_exec(Path, Cmd);
end;

end.
