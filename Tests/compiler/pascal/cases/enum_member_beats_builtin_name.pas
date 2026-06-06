program EnumMemberBeatsBuiltinName;

type
  TTile = (Wall, Floor, Exit, Treasure, Monster);

procedure SetTile(r, c : Integer; tile : TTile);
begin
  if tile = Floor then
    Writeln('PASS: floor enum')
  else
    Writeln('FAIL');
end;

begin
  SetTile(1, 2, Floor);
end.
