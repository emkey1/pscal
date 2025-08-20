program FileHandlingTortureTest;
{$APPTYPE CONSOLE}

var
  f: Text;
  s, filename: string;
  i: integer;

begin
  {--- Test 1: Write to a file ---}
  filename := 'testfile.txt';
  assign(f, filename);
  rewrite(f);
  writeln(f, 'Test File Handling');
  writeln(f, 'Line 1: Hello, world!');
  writeln(f, 'Line 2: This is a test.');
  close(f);
  writeln('Wrote to ', filename);

  {--- Test 2: Read from the file ---}
  assign(f, filename);
  reset(f);
  while not eof(f) do
  begin
    readln(f, s);
    writeln('Read from ', filename, ': ', s);
  end;
  close(f);

  {--- Test 3: Attempt to open a nonexistent file ---}
  filename := 'nonexistent_file.txt';
  assign(f, filename);
  {$I-}
  reset(f);
  {$I+}
  if IOResult <> 0 then
    writeln('Correctly detected error: Unable to open ', filename)
  else
  begin
    writeln('Error: Nonexistent file opened unexpectedly.');
    close(f);
  end;

  {--- Test 4: Write multiple lines using a loop ---}
  filename := 'multitest.txt';
  assign(f, filename);
  rewrite(f);
  for i := 1 to 10 do
    writeln(f, 'Line ', i, ': The quick brown fox jumps over the lazy dog.');
  close(f);
  writeln('Wrote multiple lines to ', filename);

  {--- Test 5: Read back the multiple lines ---}
  assign(f, filename);
  reset(f);
  i := 1;
  while not eof(f) do
  begin
    readln(f, s);
    writeln('Line ', i, ': ', s);
    i := i + 1;
  end;
  close(f);

  {--- Test 6: Mixed file operations and text processing ---}
  filename := 'case_test.txt';
  assign(f, filename);
  rewrite(f);
  writeln(f, 'MiXeD CaSe TeSt');
  close(f);
  assign(f, filename);
  reset(f);
  readln(f, s);
  close(f);
  writeln('Original text: ', s);
  writeln('Uppercase text: ');
  { Convert each character to uppercase }
  for i := 1 to length(s) do
    write(upcase(s[i]));
  writeln;

  writeln;
  writeln('File handling torture test complete.');
end.
