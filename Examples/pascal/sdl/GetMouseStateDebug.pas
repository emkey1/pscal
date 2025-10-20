#!/usr/bin/env pascal
program GetMouseStateDebug;

uses CRT;

const
  ScreenWidth  = 800;
  ScreenHeight = 600;
  Title        = 'Mouse State Debugger';
  SystemFontPath  = '/usr/local/Pscal/fonts/Roboto/static/Roboto-Regular.ttf';
  RepoFontPath    = 'fonts/Roboto/static/Roboto-Regular.ttf';
  FontSize        = 18;

var
  x, y, buttons, insideWindow : Integer;
  lastButtons, lastInside     : Integer;
  info                        : String;
  quit                        : Boolean;

function ShouldQuit: Boolean;
var
  keyCode: Integer;
begin
  ShouldQuit := False;
  keyCode := PollKeyAny();
  if keyCode <> 0 then
  begin
    if (keyCode >= 0) and (keyCode <= $FF) then
    begin
      if Chr(keyCode) in ['q', 'Q'] then
      begin
        ShouldQuit := True;
        Exit;
      end;
    end;
  end;

  if QuitRequested then
  begin
    ShouldQuit := True;
    Exit;
  end;
end;

procedure DrawStatus;
var
  statusText: String;
begin
  statusText := 'X=' + IntToStr(x) + '  Y=' + IntToStr(y) +
                '  Buttons=' + IntToStr(buttons) +
                '  Inside=' + IntToStr(insideWindow);

  SetRGBColor(20, 20, 20);
  FillRect(0, 0, GetMaxX, GetMaxY);

  if buttons <> 0 then
    SetRGBColor(255, 80, 80)
  else if insideWindow <> 0 then
    SetRGBColor(80, 180, 80)
  else
    SetRGBColor(80, 80, 180);
  FillRect(20, 20, GetMaxX - 20, GetMaxY - 20);

  SetRGBColor(255, 255, 255);
  OutTextXY(40, 40, 'Mouse State Debugger (press Q to quit)');
  OutTextXY(40, 70, statusText);
  OutTextXY(40, 100, info);
  OutTextXY(40, 130, 'Try moving/clicking inside, outside, and while the window is backgrounded.');

  UpdateScreen;
end;

procedure InitFonts;
begin
  if FileExists(SystemFontPath) then
    InitTextSystem(SystemFontPath, FontSize)
  else if FileExists(RepoFontPath) then
    InitTextSystem(RepoFontPath, FontSize)
  else
    info := 'WARNING: Roboto font not found; text output may fail.';
end;

begin
  InitGraph(ScreenWidth, ScreenHeight, Title);
  InitFonts;

  lastButtons := -1;
  lastInside := -1;
  info := 'Waiting for mouse input...';

  repeat
    GetMouseState(x, y, buttons, insideWindow);

    if (buttons <> lastButtons) or (insideWindow <> lastInside) then
    begin
      info := 'Event -> buttons=' + IntToStr(buttons) +
              ' inside=' + IntToStr(insideWindow);
      lastButtons := buttons;
      lastInside := insideWindow;
      WriteLn('[Mouse] x=', x, ' y=', y,
              ' buttons=', buttons,
              ' inside=', insideWindow);
    end;

    DrawStatus;
    GraphLoop(16);

    quit := ShouldQuit;
  until quit;

  QuitTextSystem;
  CloseGraph;

  WriteLn('GetMouseStateDebug finished.');
end.
