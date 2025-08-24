program BytecodeVerificationTest;

const
  MAX_VAL = 100;
  PI = 3.14159;
  GREETING = 'Hello, VM!';

type
  TColor = (Red, Green, Blue);
  TPoint = record
    x, y: integer;
  end;
  TVector = array[1..3] of real;
  PPoint = ^TPoint;

var
  globalInt: integer;
  globalReal: real;
  globalBool: boolean;
  globalChar: char;
  globalString: string;
  globalArray: array[0..4] of integer;
  globalRecord: TPoint;
  globalEnum: TColor;
  globalPtr: PPoint;

procedure TestParameters(var a: integer; b: integer; c: TColor);
begin
  a := a * 2;
  WriteLn('  [TestParameters] a (VAR): ', a, ', b: ', b, ', c_ord: ', ord(c));
end;

function TestFunction(x: integer): integer;
begin
  result := x * x;
end;

begin
  { Initialization }
  globalInt := 10;
  globalReal := 1.23;
  globalBool := true;
  globalChar := 'A';
  globalString := 'Initial String';
  globalEnum := Green;
  new(globalPtr);
  globalPtr^.x := 11;
  globalPtr^.y := 22;

  WriteLn('--- Initialization ---');
  WriteLn('globalInt: ', globalInt);
  WriteLn('globalReal: ', globalReal);
  WriteLn('globalBool: ', globalBool);
  WriteLn('globalChar: ', globalChar);
  WriteLn('globalString: ', globalString);
  WriteLn('globalEnum (ord): ', ord(globalEnum));
  WriteLn('globalPtr^: (', globalPtr^.x, ',', globalPtr^.y, ')');
  dispose(globalPtr);
  if globalPtr = nil then
    WriteLn('globalPtr is nil after dispose.');

  { Arithmetic and Relational Ops }
  WriteLn;
  WriteLn('--- Operators ---');
  WriteLn('10 + 5 = ', 10 + 5);
  WriteLn('10 - 5 = ', 10 - 5);
  WriteLn('10 * 5 = ', 10 * 5);
  WriteLn('10 / 4 = ', 10 / 4 :0:2);
  WriteLn('10 div 4 = ', 10 div 4);
  WriteLn('10 mod 4 = ', 10 mod 4);
  WriteLn('5 > 10 is ', 5 > 10);
  WriteLn('5 < 10 is ', 5 < 10);
  WriteLn('5 = 5 is ', 5 = 5);
  WriteLn('5 <> 5 is ', 5 <> 5);

  { Control Flow }
  WriteLn;
  WriteLn('--- Control Flow ---');
  if 10 > 5 then
    WriteLn('IF: 10 > 5');

  if 5 > 10 then
    WriteLn('IF-ELSE: This should not appear')
  else
    WriteLn('IF-ELSE: 5 is not > 10');

  globalInt := 2;
  case globalInt of
    1: WriteLn('CASE: 1');
    2: WriteLn('CASE: 2');
    3: WriteLn('CASE: 3');
  else
    WriteLn('CASE: else');
  end;

  Write('FOR loop: ');
  for globalInt := 1 to 5 do
  begin
    Write(globalInt, ' ');
    if globalInt = 3 then break;
  end;
  WriteLn;

  Write('WHILE loop: ');
  globalInt := 5;
  while globalInt > 0 do
  begin
    Write(globalInt, ' ');
    globalInt := globalInt - 1;
  end;
  WriteLn;

  Write('REPEAT loop: ');
  globalInt := 0;
  repeat
    globalInt := globalInt + 1;
    Write(globalInt, ' ');
  until globalInt = 5;
  WriteLn;

  { Procedures and Functions }
  WriteLn;
  WriteLn('--- Procedures and Functions ---');
  globalInt := 5;
  TestParameters(globalInt, 10, Blue);
  WriteLn('globalInt after TestParameters: ', globalInt);
  WriteLn('Result of TestFunction(5): ', TestFunction(5));

  { Data Structures }
  WriteLn;
  WriteLn('--- Data Structures ---');
  globalArray[0] := 10;
  globalArray[1] := 20;
  globalRecord.x := 5;
  globalRecord.y := 15;
  WriteLn('globalArray[0]: ', globalArray[0]);
  WriteLn('globalRecord.x: ', globalRecord.x);
end.
