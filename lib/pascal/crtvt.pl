unit CRTMac;

interface

const
  ESC = Chr(27);
  Black        = 0;
  Blue         = 1;
  Green        = 2;
  Cyan         = 3;
  Red          = 4;
  Magenta      = 5;
  Brown        = 6;
  LightGray    = 7;
  DarkGray     = 8;
  LightBlue    = 9;
  LightGreen   = 10;
  LightCyan    = 11;
  LightRed     = 12;
  LightMagenta = 13;
  Yellow       = 14;
  White        = 15;
  Blink        = 128;

procedure ClrScr;
procedure GotoXY(x, y: integer);
procedure ClrEol;
procedure TextColor(color: byte);
procedure TextBackground(color: byte);
procedure InvertColors;
procedure NormalColors;
procedure HideCursor;
procedure ShowCursor;
procedure Beep;
procedure Delay(ms: word);
//function MyReadKey: char;
procedure TextColorE(color: byte);
procedure TextBackgroundE(color: byte);
procedure SaveCursor;
procedure RestoreCursor;
procedure BoldText;
procedure UnderlineText;
procedure BlinkText;

implementation

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

{ Since we cannot switch the terminal to raw mode without external linking,
  MyReadKey uses the default buffered read. }
//function MyReadKey: char;
//var
//  c: char;
//begin
//  Read(c);
//  MyReadKey := c;
//end;

{ Delay uses a busy-wait loop for a crude delay.
  In the minimal runtime we provide a no-op stub to
  avoid parser issues with empty loop bodies. }
procedure Delay(ms: word);
begin
  { Intentionally left blank }
end;

procedure HideCursor;
begin
  Write(ESC, '[?25l');
end;

procedure ShowCursor;
begin
  Write(ESC, '[?25h');
end;

procedure ClrEol;
begin
  Write(ESC, '[K');
end;

procedure InvertColors;
begin
  Write(ESC, '[7m');
end;

procedure NormalColors;
begin
  Write(ESC, '[0m');
end;

procedure Beep;
begin
  Write(#7);
end;

{ Extended text color using 256-color mode }
procedure TextColorE(color: byte);
begin
  Write(ESC, '[38;5;', color, 'm');
end;

{ Extended background color using 256-color mode }
procedure TextBackgroundE(color: byte);
begin
  Write(ESC, '[48;5;', color, 'm');
end;

procedure SaveCursor;
begin
  Write(ESC, '[s');
end;

procedure RestoreCursor;
begin
  Write(ESC, '[u');
end;

procedure BoldText;
begin
  Write(ESC, '[1m');
end;

procedure UnderlineText;
begin
  Write(ESC, '[4m');
end;

procedure BlinkText;
begin
  Write(ESC, '[5m');
end;

end.
