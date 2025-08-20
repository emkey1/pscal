program ExtendedFileHandlingTests;
{$APPTYPE CONSOLE}

var
  f: Text;
  filename, line: string;
  word1, word2, word3: string;
  numLine: string;
  i: integer;

{ Test 1: Empty File Test }
procedure TestEmptyFile;
begin
  writeln('TestEmptyFile: Creating an empty file "empty.txt".');
  assign(f, 'empty.txt');
  rewrite(f);
  close(f);
  
  writeln('TestEmptyFile: Opening "empty.txt".');
  assign(f, 'empty.txt');
  reset(f);
  if eof(f) then
    writeln('TestEmptyFile: File is empty as expected.')
  else
    writeln('TestEmptyFile: File is not empty (unexpected).');
  close(f);
end;

{ Test 2: Reading Words with read }
procedure TestReadWords;
begin
  writeln('TestReadWords: Writing a line with several words to "words.txt".');
  assign(f, 'words.txt');
  rewrite(f);
  writeln(f, 'Hello World from PSCAL');
  close(f);

  writeln('TestReadWords: Opening "words.txt" and reading words using read.');
  assign(f, 'words.txt');
  reset(f);
  read(f, word1);
  read(f, word2);
  read(f, word3);
  writeln('Read words: ', word1, ' | ', word2, ' | ', word3);
  { Consume rest of the line if any }
  readln(f);
  close(f);
end;

{ Test 3: Mixed read and readln }
procedure TestMixedRead;
var
  part1, part2: string;
begin
  writeln('TestMixedRead: Writing mixed content to "mixed.txt".');
  assign(f, 'mixed.txt');
  rewrite(f);
  writeln(f, 'LineOne PartA PartB');
  writeln(f, 'LineTwo PartC PartD');
  close(f);

  writeln('TestMixedRead: Opening "mixed.txt".');
  assign(f, 'mixed.txt');
  reset(f);
  read(f, part1);       { Read first word from first line }
  readln(f, part2);     { Read remainder of first line }
  writeln('Mixed Read (Line 1): ', part1, ' -- ', part2);
  readln(f, line);      { Read entire second line }
  writeln('Mixed Read (Line 2): ', line);
  close(f);
end;

{ Test 4: Numeric and Boolean I/O }
procedure TestNumericAndBoolean;
begin
  writeln('TestNumericAndBoolean: Writing numeric and boolean values to "numbool.txt".');
  assign(f, 'numbool.txt');
  rewrite(f);
  { Write values in one line separated by spaces }
  writeln(f, 42, ' ', 3.14, ' ', true, ' ', 'TestString');
  close(f);

  writeln('TestNumericAndBoolean: Reading back from "numbool.txt".');
  assign(f, 'numbool.txt');
  reset(f);
  readln(f, numLine);
  writeln('Read line: ', numLine);
  close(f);
end;

{ Test 5: Reassign and Reuse File Variable }
procedure TestReassign;
begin
  writeln('TestReassign: Writing to "file1.txt".');
  assign(f, 'file1.txt');
  rewrite(f);
  writeln(f, 'This is file 1.');
  close(f);

  writeln('TestReassign: Reassigning f to "file2.txt".');
  assign(f, 'file2.txt');
  rewrite(f);
  writeln(f, 'This is file 2.');
  close(f);

  writeln('TestReassign: Reading back "file1.txt":');
  assign(f, 'file1.txt');
  reset(f);
  readln(f, line);
  writeln('file1.txt: ', line);
  close(f);

  writeln('TestReassign: Reading back "file2.txt":');
  assign(f, 'file2.txt');
  reset(f);
  readln(f, line);
  writeln('file2.txt: ', line);
  close(f);
end;

begin
  writeln('Starting Extended File Handling Tests...');
  writeln;
  TestEmptyFile;
  writeln;
  TestReadWords;
  writeln;
  TestMixedRead;
  writeln;
  TestNumericAndBoolean;
  writeln;
  TestReassign;
  writeln;
  writeln('Extended File Handling Tests complete.');
end.
