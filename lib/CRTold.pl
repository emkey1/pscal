unit CRT;

interface

procedure ClrScr;
procedure GotoXY(x, y: integer);
procedure TextColor(color: byte);
procedure TextBackground(color: byte);
procedure Delay(ms: word);
function MyReadKey: char;

implementation

const
  ESC = Chr(27);

{ Clears the screen and positions the cursor at the home position }
procedure ClrScr;
begin
  Write(ESC, '[2J');
  Write(ESC, '[H');
end;

{ Positions the cursor at column x, row y }
procedure GotoXY(x, y: integer);
begin
  Write(ESC, '[', y, ';', x, 'H');
end;

{ Map Turbo Pascal colors (0..15) to ANSI foreground codes }
function ForegroundCode(color: byte): string;
begin
  case color of
    0: ForegroundCode := '30';  { Black }
    1: ForegroundCode := '34';  { Blue }
    2: ForegroundCode := '32';  { Green }
    3: ForegroundCode := '36';  { Cyan }
    4: ForegroundCode := '31';  { Red }
    5: ForegroundCode := '35';  { Magenta }
    6: ForegroundCode := '33';  { Brown }
    7: ForegroundCode := '37';  { LightGray }
    8: ForegroundCode := '90';  { DarkGray }
    9: ForegroundCode := '94';  { LightBlue }
    10: ForegroundCode := '92'; { LightGreen }
    11: ForegroundCode := '96'; { LightCyan }
    12: ForegroundCode := '91'; { LightRed }
    13: ForegroundCode := '95'; { LightMagenta }
    14: ForegroundCode := '93'; { Yellow }
    15: ForegroundCode := '97'; { White }
    else ForegroundCode := '37';
  end;
end;

{ Map Turbo Pascal colors (0..15) to ANSI background codes }
function BackgroundCode(color: byte): string;
begin
  case color of
    0: BackgroundCode := '40';  { Black }
    1: BackgroundCode := '44';  { Blue }
    2: BackgroundCode := '42';  { Green }
    3: BackgroundCode := '46';  { Cyan }
    4: BackgroundCode := '41';  { Red }
    5: BackgroundCode := '45';  { Magenta }
    6: BackgroundCode := '43';  { Brown }
    7: BackgroundCode := '47';  { LightGray }
    8: BackgroundCode := '100'; { DarkGray }
    9: BackgroundCode := '104'; { LightBlue }
    10: BackgroundCode := '102';{ LightGreen }
    11: BackgroundCode := '106';{ LightCyan }
    12: BackgroundCode := '101';{ LightRed }
    13: BackgroundCode := '105';{ LightMagenta }
    14: BackgroundCode := '103';{ Yellow }
    15: BackgroundCode := '107';{ White }
    else BackgroundCode := '40';
  end;
end;

procedure TextColor(color: byte);
begin
  Write(ESC, '[', ForegroundCode(color), 'm');
end;

procedure TextBackground(color: byte);
begin
  Write(ESC, '[', BackgroundCode(color), 'm');
end;

{ Reads a single character from standard input.
  Note: This version is buffered; for immediate key response, the terminal must be set to raw mode. }
function MyReadKey: char;
var
  c: char;
begin
  Read(c);
  MyReadKey := c;
end;

{ Implements a crude busy-wait delay. Adjust the inner loop as needed. }
procedure Delay(ms: word);
var
  i, j: integer;
begin
  for i := 1 to ms do
    for j := 1 to 10000 do
      ; { Busy wait }
end;

end.
