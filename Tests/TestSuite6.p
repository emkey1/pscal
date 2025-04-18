program TestSuite;
{---------------------------------------------------------------------
  TestSuite.pas

  An enhanced self-contained test suite for pscal. In addition to testing
  constants, arithmetic, control structures, strings, records, procedure calls,
  and file I/O, this version also tests:

    - Pass by reference
    - Function calls
    - Boolean operations
    - Array access
    - FOR … DOWNTO loops
    - CASE statements
    - Built-in routines:
         * Math: cos, sin, tan, sqrt, ln, exp, abs, trunc
         * Others: copy, pos, length, inc, upcase, file I/O builtins,
                   IOResult, randomize, random
---------------------------------------------------------------------}

uses
    mylib;

const
  GREETING = 'Hello, world!';

type
  TStudent = record
    id: integer;
    name: string;
    grade: real;
  end;

var
  a, b, c: integer;
  r: real;
  s: string;
  student: TStudent;
  st: TStudent;
  f: file;

{---------------------------------------------------------------------
  Assertion Procedures
---------------------------------------------------------------------}

procedure AssertEqualInt(expected, actual: integer; testName: string);
begin
  if expected = actual then
    writeln('PASS: ', testName)
  else
    writeln('FAIL: ', testName, ' (expected: ', expected, ', got: ', actual, ')');
end;

procedure AssertEqualStr(expected, actual: string; testName: string);
begin
  if expected = actual then
    writeln('PASS: ', testName)
  else
    writeln('FAIL: ', testName, ' (expected: ', expected, ', got: ', actual, ')');
end;

procedure AssertEqualBool(expected, actual: boolean; testName: string);
begin
  if expected = actual then
    writeln('PASS: ', testName)
  else
    writeln('FAIL: ', testName, ' (expected: ', expected, ', got: ', actual, ')');
end;

procedure AssertEqualReal(expected, actual: real; testName: string);
begin
  if abs(expected - actual) < 0.0001 then
    writeln('PASS: ', testName)
  else
    writeln('FAIL: ', testName, ' (expected: ', expected:0:4, ', got: ', actual:0:4, ')');
end;

{---------------------------------------------------------------------
  Test Procedures
---------------------------------------------------------------------}

procedure TestArithmetic;
begin
  a := 10;
  b := 20;
  c := a + b;
  AssertEqualInt(30, c, 'Addition Test');
  c := b - a;
  AssertEqualInt(10, c, 'Subtraction Test');
  c := a * 3;
  AssertEqualInt(30, c, 'Multiplication Test');
  c := b div a;
  AssertEqualInt(2, c, 'Integer Division Test');
end;

procedure TestOperatorPrecedence;
var
  res: integer;
begin
  res := 2 + 3 * 4;   { Expected: 2 + (3*4) = 14 }
  AssertEqualInt(14, res, 'Operator Precedence Test');
end;

procedure TestControlStructures;
var
  i, sum: integer;
begin
  { For loop (increasing) }
  sum := 0;
  for i := 1 to 5 do
    sum := sum + i;
  AssertEqualInt(15, sum, 'For Loop Test');

  { While loop }
  i := 1;
  sum := 0;
  while i <= 5 do
  begin
    sum := sum + i;
    i := i + 1;
  end;
  AssertEqualInt(15, sum, 'While Loop Test');

  { Repeat-until loop }
  sum := 0;
  i := 1;
  repeat
    sum := sum + i;
    i := i + 1;
  until i > 5;
  AssertEqualInt(15, sum, 'Repeat Loop Test');

  { For loop (decreasing) }
  sum := 0;
  for i := 5 downto 1 do
    sum := sum + i;
  AssertEqualInt(15, sum, 'For Downto Test');
end;

procedure TestStringOperations;
begin
  s := 'Test';
  AssertEqualStr('Test', s, 'String Assignment Test');
  s := s + 'ing';
  AssertEqualStr('Testing', s, 'String Concatenation Test');
end;

procedure TestUpcase;
var
  c: char;
begin
  c := upcase('p');
  AssertEqualStr('P', upcase('p') + '', 'Upcase Test');
end;

procedure TestRecord;
begin
  st.id := 100;
  st.name := 'Alice';
  AssertEqualInt(100, st.id, 'Record Field (id) Test');
  AssertEqualStr('Alice', st.name, 'Record Field (name) Test');
end;

procedure TestArray;
var
  arr: array[1..5] of integer;
  i, sum: integer;
begin
  for i := 1 to 5 do
    arr[i] := i * 2;
  sum := 0;
  for i := 1 to 5 do
    sum := sum + arr[i];
  AssertEqualInt(30, sum, 'Array Access Test');
end;

{ Test pass-by-reference }
procedure TestPassByReference(var x: integer);
begin
  x := x + 100;
end;

procedure TestReference;
var
  i: integer;
begin
  i := 5;
  TestPassByReference(i);
  AssertEqualInt(105, i, 'Pass by Reference Test');
end;

{ Test a function call }
function Multiply(a, b: integer): integer;
begin
  Multiply := a * b;
end;

procedure TestFunction;
var
  res: integer;
begin
  res := Multiply(7, 8);
  AssertEqualInt(56, res, 'Function Test');
end;

{ Test boolean operations }
procedure TestBooleanOperations;
var
  b: boolean;
begin
  b := (5 > 3) and (3 > 2);
  AssertEqualBool(true, b, 'Boolean AND Test');

  b := (5 > 3) or (2 > 3);
  AssertEqualBool(true, b, 'Boolean OR Test');

  b := not (5 > 3);
  AssertEqualBool(false, b, 'Boolean NOT Test');
end;

{ Test case statement }
procedure TestCase;
var
  x: integer;
  resultStr: string;
begin
  x := 2;
  case x of
    1: resultStr := 'one';
    2: resultStr := 'two';
    3: resultStr := 'three';
  else
    resultStr := 'unknown';
  end;
  AssertEqualStr('two', resultStr, 'Case Statement Test');
end;

{ Test file I/O (built-in assign, rewrite, reset, close) }
procedure TestFileIO;
var
  tmp: string;
begin
  assign(f, '/tmp/testfile.txt');
  rewrite(f);
  writeln(f, 'File Test');
  close(f);

  assign(f, '/tmp/testfile.txt');
  reset(f);
  readln(f, tmp);
  close(f);
  AssertEqualStr('File Test', tmp, 'File I/O Test');
end;

{---------------------------------------------------------------------
  Builtin Tests
---------------------------------------------------------------------}

{ Math Builtins }
procedure TestMathBuiltins;
var
  res: real;
  i: integer;
begin
  res := cos(0.0);
  AssertEqualReal(1.0, res, 'Cosine of 0');

  res := sin(0.0);
  AssertEqualReal(0.0, res, 'Sine of 0');

  res := tan(0.0);
  AssertEqualReal(0.0, res, 'Tangent of 0');

  res := sqrt(4.0);
  AssertEqualReal(2.0, res, 'Square Root of 4');

  res := ln(exp(1.0));
  AssertEqualReal(1.0, res, 'Ln(exp(1))');

  res := exp(1.0);
  AssertEqualReal(exp(1.0), res, 'Exp(1)');

  res := abs(-5.5);
  AssertEqualReal(5.5, res, 'Abs(-5.5)');

  i := trunc(3.7);
  AssertEqualInt(3, i, 'Trunc(3.7)');

  i := trunc(-3.7);
  AssertEqualInt(-3, i, 'Trunc(-3.7)');
end;

{ Other Builtins }
procedure TestOtherBuiltins;
var
  str, substr: string;
  i: integer;
  f: file;
begin
  { Test copy }
  str := 'Hello, world!';
  substr := copy(str, 8, 5);  { Should yield 'world' }
  AssertEqualStr('world', substr, 'Copy Function Test');

  { Test pos }
  i := pos('world', str);
  AssertEqualInt(8, i, 'Pos Function Test');

  { Test length }
  i := length(str);
  AssertEqualInt(13, i, 'Length Function Test');

  { Test inc }
  i := 10;
  inc(i);
  AssertEqualInt(11, i, 'Inc Function Test');

  { Test upcase }
  str := upcase('a');
  AssertEqualStr('A', str, 'Upcase Function Test');

  { Test file I/O builtins (assign, rewrite, reset, close) }
  assign(f, '/tmp/temp_test.txt');
  rewrite(f);
  writeln(f, 'Test');
  close(f);
  assign(f, '/tmp/temp_test.txt');
  reset(f);
  readln(f, str);
  close(f);
  AssertEqualStr('Test', str, 'File I/O Builtins Test');

  { Test IOResult: try resetting a non-existent file }
  assign(f, '/tmp/nonexistent_file.txt');
  reset(f);
  i := ioresult;
  if i <> 0 then
    writeln('PASS: IOResult Test')
  else
    writeln('FAIL: IOResult Test');
end;

procedure PrintStudent(student: Tstudent);
begin
    if student.id = 123 then
    writeln('PASS: RecordPassing Test')
  else
    writeln('FAIL: RecordPassing Test');
end;


{---------------------------------------------------------------------
  Main Program: Run all tests
---------------------------------------------------------------------}

begin
  writeln('Running pscal Test Suite');
  TestArithmetic;
  TestOperatorPrecedence;
  TestControlStructures;
  TestStringOperations;
  TestUpcase;
  TestRecord;
  TestArray;
  TestReference;
  TestFunction;
  TestBooleanOperations;
  TestCase;
  TestFileIO;
  TestMathBuiltins;
  TestOtherBuiltins;
  student.id := 123;
  student.grade := 88.5;
  student.name := 'Bob';
  PrintStudent(student);
  writeln('Test Suite Completed');
end.
