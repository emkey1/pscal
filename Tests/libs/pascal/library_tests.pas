program PascalLibraryTests;

uses
  Base64,
  CalculateArea,
  CRT,
  MathLib,
  mylib,
  StringUtil,
  SysUtils;

var
  ExecutedTests: integer = 0;
  FailedTests: integer = 0;
  SkippedTests: integer = 0;

function RealToString(value: real): string;
begin
  RealToString := FormatFloat('0.000000', value);
end;

function JoinPath(dir, name: string): string;
begin
  if (Length(dir) = 0) then
  begin
    JoinPath := name;
    exit;
  end;
  if dir[Length(dir)] = '/' then
    JoinPath := dir + name
  else
    JoinPath := dir + '/' + name;
end;

procedure MarkPass(name: string);
begin
  ExecutedTests := ExecutedTests + 1;
  writeln('PASS ', name);
end;

procedure MarkFail(name, detail: string);
begin
  ExecutedTests := ExecutedTests + 1;
  FailedTests := FailedTests + 1;
  writeln('FAIL ', name, ': ', detail);
end;

procedure MarkSkip(name, reason: string);
begin
  SkippedTests := SkippedTests + 1;
  writeln('SKIP ', name, ': ', reason);
end;

procedure AssertTrue(name: string; condition: boolean; detail: string);
begin
  if condition then
    MarkPass(name)
  else
    MarkFail(name, detail);
end;

procedure AssertFalse(name: string; condition: boolean; detail: string);
begin
  if not condition then
    MarkPass(name)
  else
    MarkFail(name, detail);
end;

procedure AssertEqualInt(name: string; expected, actual: integer);
begin
  if expected = actual then
    MarkPass(name)
  else
    MarkFail(name, 'expected ' + IntToStr(expected) + ' but got ' + IntToStr(actual));
end;

procedure AssertEqualStr(name, expected, actual: string);
begin
  if expected = actual then
    MarkPass(name)
  else
    MarkFail(name, 'expected "' + expected + '" but got "' + actual + '"');
end;

procedure AssertFloatNear(name: string; expected, actual, tolerance: real);
var
  diff: real;
  detail: string;
begin
  diff := abs(expected - actual);
  if diff <= tolerance then
    MarkPass(name)
  else
  begin
    detail := 'expected ' + RealToString(expected) + ' +/- ' + RealToString(tolerance) +
              ' but got ' + RealToString(actual);
    MarkFail(name, detail);
  end;
end;

procedure TestCRT;
var
  originalAttr: byte;
begin
  writeln;
  writeln('-- CRT --');
  originalAttr := CRT.TextAttr;
  AssertEqualInt('CRT.Black', 0, CRT.Black);
  AssertEqualInt('CRT.Red', 4, CRT.Red);
  AssertEqualInt('CRT.White', 15, CRT.White);
  CRT.TextAttr := CRT.Green;
  AssertEqualInt('CRT.TextAttr assignment', CRT.Green, CRT.TextAttr);
  CRT.TextAttr := originalAttr;
end;

procedure TestMathLib;
begin
  writeln;
  writeln('-- MathLib --');
  AssertFloatNear('MathLib.Pi', 3.141593, MathLib.Pi, 0.000001);
  AssertFloatNear('MathLib.E', 2.718282, MathLib.E, 0.000001);
  AssertFloatNear('MathLib.PiOver2', 1.570796, MathLib.PiOver2, 0.000001);
end;

procedure TestCalculateArea;
var
  rectArea, circleArea, triArea: real;
begin
  writeln;
  writeln('-- CalculateArea --');
  rectArea := RectangleArea(5.0, 4.0);
  AssertFloatNear('RectangleArea', 20.0, rectArea, 0.0001);
  circleArea := CircleArea(2.5);
  AssertFloatNear('CircleArea', 19.634938, circleArea, 0.0001);
  triArea := TriangleArea(3.0, 4.0, 5.0);
  AssertFloatNear('TriangleArea', 6.0, triArea, 0.0001);
end;

procedure TestStringUtil;
begin
  writeln;
  writeln('-- StringUtil --');
  AssertEqualStr('ReverseString basic', 'lacsap', ReverseString('pascal'));
  AssertEqualStr('ReverseString empty', '', ReverseString(''));
  AssertEqualStr('ReverseString palindrome', 'level', ReverseString('level'));
end;

procedure TestSysUtils;
begin
  writeln;
  writeln('-- SysUtils --');
  AssertEqualStr('SysUtils.UpperCase', 'HELLO', UpperCase('hello'));
  AssertEqualStr('SysUtils.LowerCase', 'world', LowerCase('WORLD'));
  AssertEqualStr('SysUtils.Trim', 'trimmed', Trim('  trimmed  '));
  AssertEqualStr('SysUtils.TrimLeft', 'abc  ', TrimLeft('   abc  '));
  AssertEqualStr('SysUtils.TrimRight', '  abc', TrimRight('  abc   '));
  AssertEqualStr('SysUtils.QuotedStr', '''value''', QuotedStr('value'));
end;

procedure TestBase64;
var
  encoded, decoded: string;
begin
  writeln;
  writeln('-- Base64 --');
  encoded := EncodeStringBase64('hello world');
  AssertEqualStr('Base64 encode', 'aGVsbG8gd29ybGQ=', encoded);
  decoded := DecodeStringBase64(encoded);
  AssertEqualStr('Base64 roundtrip', 'hello world', decoded);
end;

procedure TestMyLib;
var
  person: TPerson;
begin
  writeln;
  writeln('-- mylib --');
  AssertEqualInt('mylib.Add', 7, Add(3, 4));
  AssertFloatNear('mylib.GetPi', 3.141590, GetPi, 0.00001);
  AssertEqualInt('mylib.GlobalCounter init', 0, GlobalCounter);
  GlobalCounter := GlobalCounter + 1;
  AssertEqualInt('mylib.GlobalCounter increment', 1, GlobalCounter);
  person.name := 'Ada';
  person.age := 36;
  AssertEqualStr('mylib.TPerson.name', 'Ada', person.name);
  AssertEqualInt('mylib.TPerson.age', 36, person.age);
end;

procedure TestFileRoundTrip(tmpDir: string);
var
  path, written, readBack: string;
  fileHandle: text;
begin
  if tmpDir = '' then
  begin
    MarkSkip('File roundtrip', 'PASCAL_TEST_TMPDIR not set');
    exit;
  end;

  writeln;
  writeln('-- File helpers --');
  path := JoinPath(tmpDir, 'pascal_library_test.txt');
  written := 'Pascal library harness';

  Assign(fileHandle, path);
  Rewrite(fileHandle);
  writeln(fileHandle, written);
  Close(fileHandle);

  Assign(fileHandle, path);
  Reset(fileHandle);
  readln(fileHandle, readBack);
  Close(fileHandle);

  AssertEqualStr('File write/read roundtrip', written, readBack);
  AssertEqualStr('Base64 file roundtrip', written, DecodeStringBase64(EncodeStringBase64(readBack)));
end;

procedure PrintSummary;
begin
  writeln;
  writeln('Executed: ', ExecutedTests);
  writeln('Failed: ', FailedTests);
  writeln('Skipped: ', SkippedTests);
end;

var
  tmpDir: string;
begin
  writeln('Pascal Library Test Suite');
  TestCRT;
  TestMathLib;
  TestCalculateArea;
  TestStringUtil;
  TestSysUtils;
  TestBase64;
  TestMyLib;

  tmpDir := GetEnv('PASCAL_TEST_TMPDIR');
  TestFileRoundTrip(tmpDir);

  PrintSummary;
  if FailedTests > 0 then
    Halt(1);
end.
