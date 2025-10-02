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

function SafeDivInt(numerator, denominator: integer): integer;
var
  quotient: double;
  truncated: integer;
begin
  if denominator = 0 then
  begin
    SafeDivInt := 0;
    exit;
  end;
  quotient := numerator / denominator;
  truncated := trunc(quotient);
  if (quotient < 0) and (quotient <> truncated) then
    SafeDivInt := truncated - 1
  else
    SafeDivInt := truncated;
end;

function MapPscalColorToAnsiBase(color: integer): integer;
var
  normalized: integer;
begin
  normalized := color mod 8;
  if normalized < 0 then
    normalized := normalized + 8;
  case normalized of
    0: MapPscalColorToAnsiBase := 0;
    1: MapPscalColorToAnsiBase := 4;
    2: MapPscalColorToAnsiBase := 2;
    3: MapPscalColorToAnsiBase := 6;
    4: MapPscalColorToAnsiBase := 1;
    5: MapPscalColorToAnsiBase := 5;
    6: MapPscalColorToAnsiBase := 3;
  else
    MapPscalColorToAnsiBase := 7;
  end;
end;

function BuildAnsiSequenceForAttr(attr: integer): string;
var
  fg, fgBase, bg, fgCode, bgCode: integer;
  isBright, isBlink: boolean;
begin
  fg := attr mod 16;
  if fg < 0 then
    fg := fg + 16;
  isBright := fg >= 8;
  fgBase := fg mod 8;

  bg := SafeDivInt(attr, 16) mod 8;
  if bg < 0 then
    bg := bg + 8;

  isBlink := (SafeDivInt(attr, 128) mod 2) = 1;

  if isBright then
    fgCode := 90 + MapPscalColorToAnsiBase(fgBase)
  else
    fgCode := 30 + MapPscalColorToAnsiBase(fgBase);
  bgCode := 40 + MapPscalColorToAnsiBase(bg);

  BuildAnsiSequenceForAttr := Chr(27) + '[0';
  if isBright then
    BuildAnsiSequenceForAttr := BuildAnsiSequenceForAttr + ';1';
  if isBlink then
    BuildAnsiSequenceForAttr := BuildAnsiSequenceForAttr + ';5';
  BuildAnsiSequenceForAttr := BuildAnsiSequenceForAttr + ';' + IntToStr(fgCode);
  BuildAnsiSequenceForAttr := BuildAnsiSequenceForAttr + ';' + IntToStr(bgCode);
  BuildAnsiSequenceForAttr := BuildAnsiSequenceForAttr + 'm';
end;

procedure ApplyTextAttrToTerminal(attr: integer);
var
  sequence: string;
begin
  sequence := BuildAnsiSequenceForAttr(attr);
  if Length(sequence) > 0 then
    write(sequence);
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
  ApplyTextAttrToTerminal(originalAttr);
end;

procedure TestMathLib;
begin
  writeln;
  writeln('-- MathLib --');
  AssertFloatNear('MathLib.Pi', 3.141593, PiValue, 0.000001);
  AssertFloatNear('MathLib.E', 2.718282, EValue, 0.000001);
  AssertFloatNear('MathLib.PiOver2', 1.570796, PiOver2Value, 0.000001);
end;

procedure TestCalculateArea;
var
  rectAreaValue, circleAreaValue, triAreaValue: real;
begin
  writeln;
  writeln('-- CalculateArea --');
  rectAreaValue := RectangleArea(5.0, 4.0);
  AssertFloatNear('RectangleArea', 20.0, rectAreaValue, 0.0001);
  circleAreaValue := CircleArea(2.5);
  AssertFloatNear('CircleArea', 19.634938, circleAreaValue, 0.0001);
  triAreaValue := TriangleArea(3.0, 4.0, 5.0);
  AssertFloatNear('TriangleArea', 6.0, triAreaValue, 0.0001);
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
  AssertFloatNear('mylib.GetPi', 3.141590, GetPi(), 0.00001);
  AssertEqualInt('mylib.GlobalCounter init', 0, GlobalCounter);
  GlobalCounter := GlobalCounter + 1;
  AssertEqualInt('mylib.GlobalCounter increment', 1, GlobalCounter);

  person.name := 'Ada Lovelace';
  person.age := 36;
  AssertEqualStr('mylib.TPerson.name', 'Ada Lovelace', person.name);
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
  originalAttr: integer;
  changedStartingAttr: boolean;
  exitCode: integer;
begin
  originalAttr := CRT.TextAttr;
  changedStartingAttr := originalAttr <> CRT.LightGray;
  if changedStartingAttr then
  begin
    CRT.TextAttr := CRT.LightGray;
    ApplyTextAttrToTerminal(CRT.LightGray);
  end
  else
    ApplyTextAttrToTerminal(originalAttr);

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
  exitCode := 0;
  if FailedTests > 0 then
    exitCode := 1;

  if changedStartingAttr then
  begin
    CRT.TextAttr := originalAttr;
    ApplyTextAttrToTerminal(originalAttr);
  end;

  if exitCode <> 0 then
    Halt(exitCode);
end.
