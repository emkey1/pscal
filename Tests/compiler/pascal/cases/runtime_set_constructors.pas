program RuntimeSetConstructors;

type
  TDir = (North, East, South, West);
  TDirSet = set of TDir;

function OppositeDir(d: TDir): TDir;
begin
  case d of
    North: OppositeDir := South;
    South: OppositeDir := North;
    East: OppositeDir := West;
    West: OppositeDir := East;
  end;
end;

var
  walls: TDirSet;
  d: TDir;
  lo: TDir;
  hi: TDir;

begin
  walls := [North, East, South, West];
  d := East;
  walls := walls - [d];
  walls := walls - [OppositeDir(d)];

  lo := North;
  hi := South;
  walls := [lo..hi];

  if (North in walls) and (East in walls) and (South in walls) and not (West in walls) then
    Writeln('PASS: runtime set constructors')
  else
    Writeln('FAIL: runtime set constructors');
end.
