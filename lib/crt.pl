unit CRT;

interface

const
  ESC = Chr(27);
  // Standard Turbo Pascal Color Constants
  Black        = 0;
  Blue         = 1;
  Green        = 2;
  Cyan         = 3;
  Red          = 4;
  Magenta      = 5;
  Brown        = 6; // Often Yellow/Orange in ANSI
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

// --- Standard CRT Procedures/Functions ---
procedure ClrScr;
procedure GotoXY(x, y: integer);
procedure ClrEol;
procedure TextColor(color: byte);
procedure TextBackground(color: byte);
procedure Delay(ms: word);
procedure HideCursor;
procedure CursorOff;
procedure ShowCursor;
procedure CursorOn;
procedure DelLine; 
procedure InsLine;
procedure Beep;
procedure NormVideo;
procedure HighVideo;
procedure LowVideo;
procedure Window(x1, y1, x2, y2: integer);
function WhereX: integer;
function WhereY: integer;
function KeyPressed: boolean;

// --- Extended/Helper Procedures/Functions 
procedure InvertColors;
procedure NormalColors; // Note: Same effect as NormVideo
procedure TextColorE(color: byte);
procedure TextBackgroundE(color: byte);
procedure SaveCursor;
procedure RestoreCursor;
procedure BoldText;      // Note: Same effect as HighVideo
procedure UnderlineText;
procedure BlinkText;

var
  // Global variables to store the current window coordinates
  WinLeft, WinTop, WinRight, WinBottom: integer;
  // Default text attributes can be stored here if needed
  // e.g., CurrentTextColor, CurrentTextBackground: byte;

implementation

const
  foo        = 128; // Placeholder


{ Clears the screen (or current window) and positions the cursor at the top-left of the window }
procedure ClrScr;
begin
  // Note: Standard ANSI ClrScr [2J clears entire screen.
  // To clear only within the window requires more complex logic
  // (e.g., looping through window lines and using ClrEol) or
  // using DECSED which might not be universally supported.
  // We'll clear the whole screen and home the cursor to window top-left.
  Write(ESC, '[2J');
  GotoXY(WinLeft, WinTop); // Home cursor to window top-left
end;

{ Positions the cursor at column x, row y relative to the current window }
procedure GotoXY(x, y: integer);
var
  absX, absY: integer;
begin
  // Calculate absolute screen coordinates based on window
  absX := WinLeft + x - 1;
  absY := WinTop + y - 1;

  // Clamp to window boundaries (optional, but good practice)
  // if absX < WinLeft then absX := WinLeft;
  // if absX > WinRight then absX := WinRight;
  // if absY < WinTop then absY := WinTop;
  // if absY > WinBottom then absY := WinBottom;

  // Note: Actual cursor positioning respects window, but text wrapping might not
  Write(ESC, '[', absY, ';', absX, 'H');
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

procedure HideCursor;
begin
  Write(ESC, '[?25l');
end;

procedure CursorOff;
begin
  Write(ESC, '[?25l');
end;

procedure ShowCursor;
begin
  Write(ESC, '[?25h');
end;

procedure CursorOn;
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

procedure TextColorE(color: byte);
begin
  Write(ESC, '[38;5;', color, 'm');
end;

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

function WhereX: integer;
begin
  WhereX := BIWhereX; // Assumes BIWhereX is a built-in returning X coord
end;

function WhereY: integer;
begin
  WhereY := BIWhereY; // Assumes BIWhereY is a built-in returning Y coord
end;

// --- NEWLY ADDED FUNCTIONS ---

{ Deletes the line containing the cursor and moves lines below it up }
procedure DelLine;
begin
  Write(ESC, '[M');
end;

{ Inserts a blank line at the cursor position and moves lines below it down }
procedure InsLine;
begin
  Write(ESC, '[L');
end;

{ Sets text attribute to high intensity (bold) }
procedure HighVideo;
begin
  TextColor(DarkGray); // Common interpretation of LowVideo
  // Alternatively, directly: Write(ESC, '[1m');
end;

{ Sets text attribute to low intensity (often dark gray foreground) }
procedure LowVideo;
begin
  BoldText; // Reuse existing BoldText procedure
  // Alternatively, could use faint code if supported: Write(ESC, '[2m');
end;

{ Sets text attribute to the default/normal intensity and colors }
procedure NormVideo;
begin
  NormalColors; // Reuse existing NormalColors procedure
  // Alternatively, directly: Write(ESC, '[0m');
end;

{ Defines a text window on the screen }
procedure Window(x1, y1, x2, y2: integer);
begin
  // Basic input validation
  if (x1 < 1) or (y1 < 1) or (x2 < x1) or (y2 < y1) then
  begin
// Should handle the invalid case
  end
  else // Only set window if coordinates seem valid
  begin
    // Store window coordinates (adjusting for 1-based Pascal coords)
    WinLeft := x1;
    WinTop := y1;
    WinRight := x2;
    WinBottom := y2;

    // Use ANSI sequence to set Top and Bottom Margins (Scrolling Region)
    // Note: This controls SCROLLING, but does NOT prevent writing outside
    // the horizontal bounds (x1, x2) or clip ClrScr horizontally.
    Write(ESC, '[', WinTop, ';', WinBottom, 'r');

    // Move cursor to top-left of the new window
    GotoXY(1, 1);
  end;
end;

// --- Initialization block for the unit ---
begin
  // Set default window to full screen (assuming 80x24 typical size)
  // Adjust these defaults if your terminal size is different
  WinLeft := 1;
  WinTop := 1;
  WinRight := 80;
  WinBottom := 24;
  // Optionally set default colors here too
  // NormalColors;
end.
