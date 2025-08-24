unit CRT;

interface

{ All CRT routines are now provided as VM builtins. }

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

type
  TOSType = (osUnknown, osLinux, osMac);

implementation
begin
end.
