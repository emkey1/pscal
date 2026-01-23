program CRTLabeledSmoke;
{ A lightweight CRT exercise to sanity-check console primitives without
  relying on interactive inspection. It emits labeled lines so automated
  runs can assert behavior (e.g., WhereX/WhereY values) while still driving
  the CRT routines that manipulate the terminal state. }

uses CRT;

var
  x, y: integer;
  savedAttr: byte;
begin
  { Baseline clear and cursor query. }
  ClrScr;
  GotoXY(5, 2);
  Write('ABC');
  ClrEol;
  x := WhereX; y := WhereY;
  writeln('[cursor]', x, ',', y);

  { Save/restore cursor. }
  SaveCursor;
  GotoXY(10, 3);
  Write('POS');
  RestoreCursor;
  writeln('[restore]', WhereX, ',', WhereY);

  { Windowed clearing should stay inside the defined region. }
  Window(2, 2, 10, 4);
  ClrScr;
  GotoXY(1, 1); Write('WIN');
  GotoXY(1, 3); Write('END');
  Window(1, 1, 80, 24); { restore full screen }
  writeln('[window]', WhereX, ',', WhereY);

  { Color and TextAttr round-trip. }
  savedAttr := TextAttr;
  TextColor(Red);
  TextBackground(Blue);
  BoldText;
  writeln('[colors-set]', TextAttr);
  TextAttr := savedAttr;
  writeln('[colors-restored]', TextAttr);

  { Alternate screen swap should preserve primary contents. }
  PushScreen;
  ClrScr;
  GotoXY(1, 1); Write('ALT');
  PopScreen;
  writeln('[altscreen]done');

  { Cursor visibility toggles should not hang. }
  HideCursor;
  ShowCursor;

  { Beep + tiny delay for completeness. }
  Beep;
  Delay(10);
end.
