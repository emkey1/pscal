#!/usr/bin/env pascal
program DosExample;
uses dos;
var
  Year, Month, Day, DayOfWeek : Word;
  Hour, Min, Sec, Sec100 : Word;
begin
  GetDate(Year, Month, Day, DayOfWeek);
  GetTime(Hour, Min, Sec, Sec100);
  writeln('Today is: ', Month, '/', Day, '/', Year);
  writeln('Current time is: ', Hour, ':', Min, ':', Sec);
  writeln('PATH environment variable is: ', GetEnv('PATH'));
end.
