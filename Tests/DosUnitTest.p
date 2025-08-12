program DosUnitTest;
uses Dos;

var
  f1, f2, f3: string;
  attr: integer;
  year, month, day, dow: word;
  hour, minute, second, sec100: word;
  execRes: integer;
  f: text;

begin
  MkDir('dostest');
  if GetFAttr('dostest') = 16 then
    writeln('MKDIR PASS')
  else
    writeln('MKDIR FAIL');

  assign(f, 'dostest/a.txt'); rewrite(f); writeln(f, 'A'); close(f);
  assign(f, 'dostest/b.txt'); rewrite(f); writeln(f, 'B'); close(f);

  f1 := FindFirst('dostest');
  f2 := FindNext;
  f3 := FindNext;
  if (((f1 = 'a.txt') and (f2 = 'b.txt')) or ((f1 = 'b.txt') and (f2 = 'a.txt'))) and (f3 = '') then
    writeln('FIND PASS')
  else
    writeln('FIND FAIL');

  attr := GetFAttr('dostest/a.txt');
  if attr = 0 then
    writeln('GETFATTR FILE PASS')
  else
    writeln('GETFATTR FILE FAIL');

  attr := GetFAttr('dostest');
  if attr = 16 then
    writeln('GETFATTR DIR PASS')
  else
    writeln('GETFATTR DIR FAIL');

  if (GetEnv('HOME') <> '') and (GetEnv('PSCAL_DOES_NOT_EXIST') = '') then
    writeln('GETENV PASS')
  else
    writeln('GETENV FAIL');

  GetDate(year, month, day, dow);
  if (year > 2000) and (month >= 1) and (month <= 12) and (day >= 1) and (day <= 31) and (dow <= 6) then
    writeln('GETDATE PASS')
  else
    writeln('GETDATE FAIL');

  GetTime(hour, minute, second, sec100);
  if (hour <= 23) and (minute <= 59) and (second <= 59) and (sec100 <= 99) then
    writeln('GETTIME PASS')
  else
    writeln('GETTIME FAIL');

  execRes := Exec('/bin/sh', '-c "echo hi > dostest/exec.out"');
  if (execRes = 0) and (GetFAttr('dostest/exec.out') = 0) then
    writeln('EXEC PASS')
  else
    writeln('EXEC FAIL');

  Exec('/bin/sh', '-c "rm dostest/a.txt dostest/b.txt dostest/exec.out"');
  RmDir('dostest');
  if GetFAttr('dostest') = 0 then
    writeln('RMDIR PASS')
  else
    writeln('RMDIR FAIL');
end.
