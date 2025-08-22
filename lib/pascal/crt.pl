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

type
  TOSType = (osUnknown, osLinux, osMac);

// --- Standard CRT Procedures/Functions ---
//procedure ClrScr;
procedure GotoXY(x, y: integer);
procedure ClrEol;
//procedure TextColor(color: byte); // Now built in
//procedure TextBackground(color: byte); // Now built in
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
//procedure TextColorE(color: byte);       // <--- DECLARATION ONLY (Uses 256 colors via C built-in)
//procedure TextBackgroundE(color: byte); // <--- DECLARATION ONLY (Uses 256 colors via C built-in)
procedure SaveCursor;
procedure RestoreCursor;
procedure BoldText;      // Note: Same effect as HighVideo
procedure UnderlineText;
procedure BlinkText;

// --- Environment Helpers ---
function GetEnv(VarName: string): string;
function GetEnvInt(const Name: string; Default: integer): integer;
procedure Val(S: string; var N: integer; var Code: integer);
procedure ValReal(S: string; var R: real; var Code: integer);

var
  // Global variables to store the current window coordinates
  WinLeft, WinTop, WinRight, WinBottom: integer;
  OSKind: TOSType;
  // Default text attributes can be stored here if needed
  // e.g., CurrentTextColor, CurrentTextBackground: byte;

implementation

const
  foo        = 128; // Placeholder


{ Clears the screen (or current window) and positions the cursor at the top-left of the window }
//procedure ClrScr;
//begin
//  case OSKind of
//    osLinux, osMac:
//      begin
//        BIClrScr;
//        GotoXY(WinLeft, WinTop);
//      end;
//  else
//      begin
//        Write(#12);
//        GotoXY(1, 1);
//      end;
//  end;
//end;

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

//procedure TextColorE(color: byte);
//begin
//  Write(ESC, '[38;5;', color, 'm');
//end;

//procedure TextBackgroundE(color: byte);
//begin
//  Write(ESC, '[48;5;', color, 'm');
//end;

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
  BIBoldText;
end;

procedure UnderlineText;
begin
  BIUnderlineText;
end;

procedure BlinkText;
begin
  BIBlinkText;
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
  BIBoldText;
end;

{ Sets text attribute to low intensity (often dark gray foreground) }
procedure LowVideo;
begin
  BILowVideo;
end;

{ Sets text attribute to the default/normal intensity and colors }
procedure NormVideo;
begin
  BINormVideo;
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

function GetEnv(VarName: string): string;
begin
  GetEnv := dos_getenv(VarName);
end;

{ Convert a string to an integer. Mimics Turbo Pascal's Val behaviour. }
procedure Val(S: string; var N: integer; var Code: integer);
var
  i, len, sign: integer;
  digitFound: boolean;
begin
  N := 0;
  Code := 0;
  len := Length(S);
  if len = 0 then
  begin
    Code := 1;
    exit;
  end;
  i := 1;
  while (i <= len) and (S[i] <= ' ') do
    i := i + 1;
  if i > len then
  begin
    Code := 1;
    exit;
  end;
  sign := 1;
  if (S[i] = '+') or (S[i] = '-') then
  begin
    if S[i] = '-' then
      sign := -1;
    i := i + 1;
  end;
  if i > len then
  begin
    Code := i;
    exit;
  end;
  digitFound := False;
  while (i <= len) and (S[i] >= '0') and (S[i] <= '9') do
  begin
    digitFound := True;
    N := N * 10 + Ord(S[i]) - Ord('0');
    i := i + 1;
  end;
  if not digitFound then
  begin
    Code := i;
    N := 0;
    exit;
  end;
  if i <= len then
  begin
    Code := i;
    exit;
  end;
  N := N * sign;
end;

{ Convert a string to a real number. Supports optional decimal and exponent. }
procedure ValReal(S: string; var R: real; var Code: integer);
var
  i, len, sign, expSign, expVal: integer;
  intPart, fracPart, fracDiv: real;
  digitFound: boolean;
begin
  R := 0.0;
  Code := 0;
  len := Length(S);
  if len = 0 then
  begin
    Code := 1;
    exit;
  end;
  i := 1;
  while (i <= len) and (S[i] <= ' ') do
    i := i + 1;
  if i > len then
  begin
    Code := 1;
    exit;
  end;
  sign := 1;
  if (S[i] = '+') or (S[i] = '-') then
  begin
    if S[i] = '-' then
      sign := -1;
    i := i + 1;
  end;
  if i > len then
  begin
    Code := i;
    exit;
  end;
  digitFound := False;
  intPart := 0.0;
  while (i <= len) and (S[i] >= '0') and (S[i] <= '9') do
  begin
    digitFound := True;
    intPart := intPart * 10.0 + (Ord(S[i]) - Ord('0'));
    i := i + 1;
  end;
  R := intPart;
  if (i <= len) and (S[i] = '.') then
  begin
    i := i + 1;
    fracPart := 0.0;
    fracDiv := 1.0;
    if (i > len) or not ((S[i] >= '0') and (S[i] <= '9')) then
    begin
      Code := i;
      exit;
    end;
    while (i <= len) and (S[i] >= '0') and (S[i] <= '9') do
    begin
      digitFound := True;
      fracPart := fracPart * 10.0 + (Ord(S[i]) - Ord('0'));
      fracDiv := fracDiv * 10.0;
      i := i + 1;
    end;
    R := R + fracPart / fracDiv;
  end;
  if (i <= len) and ((S[i] = 'E') or (S[i] = 'e')) then
  begin
    i := i + 1;
    expSign := 1;
    if (i <= len) and (S[i] = '+') then
      i := i + 1
    else if (i <= len) and (S[i] = '-') then
    begin
      expSign := -1;
      i := i + 1;
    end;
    if (i > len) or not ((S[i] >= '0') and (S[i] <= '9')) then
    begin
      Code := i;
      exit;
    end;
    expVal := 0;
    while (i <= len) and (S[i] >= '0') and (S[i] <= '9') do
    begin
      expVal := expVal * 10 + Ord(S[i]) - Ord('0');
      i := i + 1;
    end;
    if expSign = 1 then
      while expVal > 0 do
      begin
        R := R * 10.0;
        expVal := expVal - 1;
      end
    else
      while expVal > 0 do
      begin
        R := R / 10.0;
        expVal := expVal - 1;
      end;
  end;
  if not digitFound then
  begin
    Code := i;
    R := 0.0;
    exit;
  end;
  if i <= len then
  begin
    Code := i;
    R := 0.0;
    exit;
  end;
  R := R * sign;
end;

function GetEnvInt(const Name: string; Default: integer): integer;
var
  S: string;
  Code, N: integer;
begin
  S := GetEnv(Name);
  if S <> '' then
  begin
    Val(S, N, Code);
    if Code = 0 then
      GetEnvInt := N
    else
      GetEnvInt := Default;
  end
  else
    GetEnvInt := Default;
end;

function DetectOS: TOSType;
var
  S: string;
  I: integer;
begin
  S := GetEnv('OSTYPE');
  for I := 1 to Length(S) do
    S[I] := UpCase(S[I]);
  if Pos('LINUX', S) > 0 then
    DetectOS := osLinux
  else if Pos('DARWIN', S) > 0 then
    DetectOS := osMac
  else
    DetectOS := osUnknown;
end;

// --- Initialization block for the unit ---
begin
  OSKind := DetectOS;

  // Set default window to full screen using environment size when available
  WinLeft := 1;
  WinTop := 1;
  WinRight := GetEnvInt('COLUMNS', 80);
  WinBottom := GetEnvInt('LINES', 24);
  // Optionally set default colors here too
  // NormalColors;
end.
