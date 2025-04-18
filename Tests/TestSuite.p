program TestSuite;
{---------------------------------------------------------------------
  TestSuite.pas

  A self-contained test suite for pscal. This program exercises a variety
  of language features including constants, arithmetic, control structures,
  strings, records, procedure calls, and Turbo Pascal–style file I/O.

  Each sub-test calls an assertion procedure to compare the expected result
  with the actual result and then prints "PASS" or "FAIL" along with a message.

  To run the test suite, load it into pscal and execute it.
---------------------------------------------------------------------}

const
    PI = 314;             { A constant (approximation: 3.14*100) }
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
    f: file;             { File variable (declared as file or Text) }
    
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
    { For loop test: sum 1 to 5 should be 15 }
    sum := 0;
    for i := 1 to 5 do
         sum := sum + i;
    AssertEqualInt(15, sum, 'For Loop Test');

    { While loop test }
    i := 1;
    sum := 0;
    while i <= 5 do
    begin
         sum := sum + i;
         i := i + 1;
    end;
    AssertEqualInt(15, sum, 'While Loop Test');

    { Repeat-until loop test }
    sum := 0;
    i := 1;
    repeat
         sum := sum + i;
         i := i + 1;
    until i > 5;
    AssertEqualInt(15, sum, 'Repeat Loop Test');
end;

procedure TestStringOperations;
begin
    s := 'Test';
    AssertEqualStr('Test', s, 'String Assignment Test');
    s := s + 'ing';
    AssertEqualStr('Testing', s, 'String Concatenation Test');
end;

procedure TestRecord;
begin
    st.id := 100;
    st.name := 'Alice';
    AssertEqualInt(100, st.id, 'Record Field (id) Test');
    AssertEqualStr('Alice', st.name, 'Record Field (name) Test');
end;

procedure TestProcedureParameter(x: integer);
begin
    AssertEqualInt(x + x, 2 * x, 'Procedure Parameter Test');
end;

procedure TestProcedureCalls;
begin
    TestProcedureParameter(50);
end;

procedure TestFileIO;
var
    tmp: string;
begin
    { Write to a file }
    assign(f, 'testfile.txt');
    rewrite(f);
    writeln(f, 'File Test');
    close(f);

    { Read from the file }
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
    TestRecord;
    TestProcedureCalls;
    TestFileIO;
    writeln('Test Suite Completed');
end.

