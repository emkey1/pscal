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
    - The built-in upcase function
---------------------------------------------------------------------}

const
    PI = 314;             { Approximation for 3.14*100 }
    GREETING = 'Hello, world!';

type
    TStudent = record
         id: integer;
         name: string;
    end;

var
    a, b, c: integer;
    r: real;
    s: string;
    st: TStudent;
    f: file;             { File variable }

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
    c: string;
begin
    c := upcase('p');
    AssertEqualStr('P', c, 'Upcase Test');
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

{ Test random functionality }
procedure TestRandom;
var
    n: integer;
    r: real;
begin
    n := random(10);
    if (n < 0) or (n >= 10) then
      writeln('FAIL: Random Integer Test (out of bounds)')
    else
      writeln('PASS: Random Integer Test');
    r := random;
    if (r < 0.0) or (r >= 1.0) then
      writeln('FAIL: Random Real Test (out of bounds)')
    else
      writeln('PASS: Random Real Test');
end;

{ Test file I/O }
procedure TestFileIO;
var
    tmp: string;
begin
    assign(f, 'testfile.txt');
    rewrite(f);
    writeln(f, 'File Test');
    close(f);

    assign(f, 'testfile.txt');
    reset(f);
    readln(f, tmp);
    close(f);
    AssertEqualStr('File Test', tmp, 'File I/O Test');
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
    TestRandom;
    TestFileIO;
    writeln('Test Suite Completed');
end.

