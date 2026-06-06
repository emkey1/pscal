program TextFileAliasBasic;

var
  f: TextFile;
begin
  AssignFile(f, 'textfile_alias_basic.tmp');
  Rewrite(f);
  WriteLn(f, 'PASS: textfile alias');
  CloseFile(f);
  WriteLn('PASS: textfile alias');
end.
