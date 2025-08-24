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
function MkDir(Path: string): integer;
function RmDir(Path: string): integer;
function GetEnv(VarName: string): string;
procedure dos_getdate(var Year, Month, Day, Dow: word);
procedure dos_gettime(var Hour, Minute, Second, Sec100: word);
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

function MkDir(Path: string): integer;
begin
  MkDir := dos_mkdir(Path);
end;

function RmDir(Path: string): integer;
begin
  RmDir := dos_rmdir(Path);
end;

function GetEnv(VarName: string): string;
begin
  GetEnv := dos_getenv(VarName);
end;

function Exec(Path, Cmd: string): integer;
begin
  Exec := dos_exec(Path, Cmd);
end;

end.
