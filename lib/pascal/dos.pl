unit Dos;

interface

type
  SearchRec = record
    Name: string;
    Attr: integer;
  end;

function findFirst(Path: string): string; (* returns first entry name, empty when none *)
function findNext: string; (* returns next entry name *)
function getFAttr(Path: string): integer;
function mkDir(Path: string): integer;
function rmDir(Path: string): integer;
function getEnv(VarName: string): string;
procedure getDate(var Year, Month, Day, Dow: word);
procedure getTime(var Hour, Minute, Second, Sec100: word);
function exec(Path, Cmd: string): integer;

implementation
function findFirst(Path: string): string;
begin
  findFirst := dosFindfirst(Path);
end;

function findNext: string;
begin
  findNext := dosFindnext();
end;

function getFAttr(Path: string): integer;
begin
  getFAttr := dosGetfattr(Path);
end;

function mkDir(Path: string): integer;
begin
  mkDir := dosMkdir(Path);
end;

function rmDir(Path: string): integer;
begin
  rmDir := dosRmdir(Path);
end;

function getEnv(VarName: string): string;
begin
  getEnv := dosGetenv(VarName);
end;

procedure getDate(var Year, Month, Day, Dow: word);
begin
  dosGetdate(Year, Month, Day, Dow);
end;

procedure getTime(var Hour, Minute, Second, Sec100: word);
begin
  dosGettime(Hour, Minute, Second, Sec100);
end;

function exec(Path, Cmd: string): integer;
begin
  exec := dosExec(Path, Cmd);
end;

end.
