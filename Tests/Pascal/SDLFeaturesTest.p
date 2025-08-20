#!/usr/bin/env pscal
PROGRAM SDLFeaturesTest;

USES CRT; // For console WriteLn and ReadKey

CONST
  ScreenWidth  = 1024;
  ScreenHeight = 768;
  Title        = 'Pscal SDL Features Test';
  FontPath     = '/usr/local/Pscal/fonts/Roboto/static/Roboto-Regular.ttf'; // Make sure this path is correct for your system
  FontSize     = 18;

  // Mouse button constants (example mapping)
  ButtonLeft   = 1;
  ButtonMiddle = 2;
  ButtonRight  = 4;

VAR
  mx, my, buttons : Integer;
  quit            : Boolean;
  eventInfo       : String;
  frameCount      : Integer;
  r, g, b         : Byte;

BEGIN
  InitGraph(ScreenWidth, ScreenHeight, Title);
  InitTextSystem(FontPath, FontSize); // Initialize font system

  quit := False;
  frameCount := 0;
  eventInfo := 'Move mouse and click... Press Q to quit.';

  // Main loop
  WHILE NOT quit DO
  BEGIN
    frameCount := frameCount + 1;

    // Get mouse state
    GetMouseState(mx, my, buttons);

    // --- Background ---
    SetRGBColor(30, 30, 50); // Dark blue background
    FillRect(0, 0, GetMaxX, GetMaxY);

    // --- Draw a Line that follows the mouse Y ---
    SetRGBColor(100, 200, 255); // Light blue
    DrawLine(0, my, GetMaxX, my);

    // --- Draw a Rectangle that follows the mouse X ---
    SetRGBColor(255, 100, 100); // Light red
    FillRect(mx - 20, 50, mx + 20, 100); // A small rectangle

    // --- Draw a Circle that changes radius with mouse X ---
    // Ensure MOD results are always positive and in range 0..255
    r := ((frameCount DIV 2) MOD 256 + 256) MOD 256;
    g := ((100 + frameCount DIV 3) MOD 256 + 256) MOD 256;
    b := ((200 - frameCount DIV 4) MOD 256 + 256) MOD 256; // <<<< CORRECTED LINE
    SetRGBColor(r,g,b);
    DrawCircle(GetMaxX DIV 2, GetMaxY DIV 2, (mx MOD 100) + 20);

    // --- Text Output ---
    SetRGBColor(255, 255, 0); // Yellow text
    OutTextXY(10, 10, 'Pscal SDL Test Program!');
    OutTextXY(10, 30, 'Mouse X: ' + IntToStr(mx) + ' Y: ' + IntToStr(my));
    OutTextXY(10, 50, 'Buttons: ' + IntToStr(buttons));
    OutTextXY(10, ScreenHeight - 30, eventInfo);

    // --- Handle Mouse Clicks for simple interaction ---
    IF (buttons AND ButtonLeft) <> 0 THEN
    BEGIN
      SetRGBColor(0, 255, 0); // Green
      FillRect(mx - 5, my - 5, mx + 5, my + 5);
      eventInfo := 'Left Button Clicked!';
    END
    ELSE IF (buttons AND ButtonRight) <> 0 THEN
    BEGIN
      SetRGBColor(255, 0, 0); // Red
      DrawCircle(mx, my, 15);
      eventInfo := 'Right Button Clicked!';
    END
    ELSE IF (buttons AND ButtonMiddle) <> 0 THEN
    BEGIN
       eventInfo := 'Middle Button Clicked!';
    END;

    UpdateScreen;
    GraphLoop(16);

    IF KeyPressed THEN
    BEGIN
      IF UpCase(ReadKey) = 'Q' THEN
        quit := True;
    END;

  END; // WHILE NOT quit

  QuitTextSystem;
  CloseGraph;

  WriteLn('Test program finished.');
END.
