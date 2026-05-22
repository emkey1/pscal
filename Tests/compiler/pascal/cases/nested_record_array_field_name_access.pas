program NestedRecordArrayFieldNameAccess;

const
  MaxWidth = 120;
  MaxHeight = 50;
  MaxEnemies = 20;
  MaxDepth = 8;

type
  TMap = array[1..MaxHeight, 1..MaxWidth] of Char;

  TEnemy = record
    X, Y: Integer;
    HP, MaxHP: Integer;
    Damage: Integer;
    Glyph: Char;
    Speed, Energy, Color: Integer;
    Active: Boolean;
    Name: String[15];
  end;

  TLevel = record
    Generated: Boolean;
    Map: TMap;
    Seen: TMap;
    Enemies: array[1..MaxEnemies] of TEnemy;
  end;

var
  Levels: array[1..MaxDepth] of TLevel;
  L, R, C: Integer;

begin
  L := 2;
  Levels[L].Generated := True;

  for R := 1 to MaxHeight do
    for C := 1 to MaxWidth do
    begin
      Levels[L].Map[R, C] := '.';
      Levels[L].Seen[R, C] := ' ';
    end;

  Levels[L].Enemies[1].X := 12;
  Levels[L].Enemies[1].Y := 34;
  Levels[L].Enemies[1].HP := 5;
  Levels[L].Enemies[1].MaxHP := 9;
  Levels[L].Enemies[1].Damage := 3;
  Levels[L].Enemies[1].Glyph := 'r';
  Levels[L].Enemies[1].Speed := 100;
  Levels[L].Enemies[1].Energy := 7;
  Levels[L].Enemies[1].Color := 6;
  Levels[L].Enemies[1].Active := True;
  Levels[L].Enemies[1].Name := 'Rat';

  WriteLn(Levels[L].Enemies[1].X, ' ', Levels[L].Enemies[1].Y, ' ',
          Levels[L].Enemies[1].HP, ' ', Levels[L].Enemies[1].MaxHP, ' ',
          Levels[L].Enemies[1].Damage, ' ', Levels[L].Enemies[1].Glyph, ' ',
          Levels[L].Enemies[1].Speed, ' ', Levels[L].Enemies[1].Energy, ' ',
          Levels[L].Enemies[1].Color, ' ', Ord(Levels[L].Enemies[1].Active), ' ',
          Levels[L].Enemies[1].Name);
  WriteLn('PASS: nested record-array field name access');
end.
