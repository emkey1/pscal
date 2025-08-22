#!/usr/bin/env pascal
program CRTFunctionsDemo;

uses CRT;

var
  ch: char;
  x, y: integer;

begin
  ClrScr;
  writeln('CRT function demo. Press any key to begin...');
  ch := ReadKey;

  { Color and background }
  TextColor(Yellow);
  TextBackground(Blue);
  writeln('Colored text on blue background');
  NormalColors;
  Delay(500);

  { Cursor visibility }
  writeln('Hiding cursor for a moment...');
  HideCursor;
  Delay(500);
  ShowCursor;
  writeln('cursor visible again');

  { Beep }
  writeln('About to beep...');
  Delay(500);
  Beep;

  { Save and restore cursor }
  write('Saving cursor position');
  SaveCursor;
  GotoXY(40,5);
  write('Moved here');
  Delay(500);
  RestoreCursor;
  writeln(' and restored');

  { Report cursor position }
  x := WhereX;
  y := WhereY;
  writeln('Current cursor position: X=', x, ' Y=', y);

  { Clear to end of line }
  write('This text will disappear');
  Delay(500);
  GotoXY(x, y+1);
  write('Line to clear...');
  Delay(500);
  GotoXY(x, y+1);
  ClrEol;

  { Delete and insert line }
  writeln('Line 1');
  writeln('Line 2');
  writeln('Line 3');
  y := WhereY; { store next line }
  GotoXY(1, y-2); { move to line 2 }
  DelLine;
  InsLine;
  writeln('Inserted line');

  { Window demo }
  Delay(500);
  Window(30,5,60,10);
  ClrScr;
  writeln('Inside a small window');
  Delay(500);
  Window(1,1,80,25);

  { Text attributes }
  HighVideo; writeln('HighVideo');
  LowVideo; writeln('LowVideo');
  NormVideo; writeln('NormVideo');
  BoldText; writeln('BoldText');
  UnderlineText; writeln('UnderlineText');
  BlinkText; writeln('BlinkText');
  NormVideo;
  InvertColors; writeln('Inverted colors');
  NormalColors;

  writeln('Press any key to exit.');
  ch := ReadKey;
  ShowCursor;
  NormalColors;
  ClrScr;
end.
