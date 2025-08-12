program MathLibTest;

uses
  MathLib;

const
  TOL = 0.0001;

var
  r: real;
  i: integer;

procedure TestMathLib;
begin
  writeln;
  writeln('--- Testing MathLib ---');

  r := ArcTan(0.5);
  write('START: ArcTan(0.5): ');
  if abs(0.4636 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 0.4636:0:4, ', got: ', r:0:4, ')');

  r := ArcSin(0.5);
  write('START: ArcSin(0.5): ');
  if abs(0.5236 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 0.5236:0:4, ', got: ', r:0:4, ')');

  r := ArcCos(0.5);
  write('START: ArcCos(0.5): ');
  if abs(1.0472 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 1.0472:0:4, ', got: ', r:0:4, ')');

  r := Cotan(0.785398);
  write('START: Cotan(pi/4): ');
  if abs(1.0 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 1.0:0:4, ', got: ', r:0:4, ')');

  r := Power(2.0, 3.0);
  write('START: Power(2,3): ');
  if abs(8.0 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 8.0:0:4, ', got: ', r:0:4, ')');

  r := Log10(1000.0);
  write('START: Log10(1000): ');
  if abs(3.0 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 3.0:0:4, ', got: ', r:0:4, ')');

  r := Sinh(0.0);
  write('START: Sinh(0): ');
  if abs(0.0 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 0.0:0:4, ', got: ', r:0:4, ')');

  r := Cosh(0.0);
  write('START: Cosh(0): ');
  if abs(1.0 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 1.0:0:4, ', got: ', r:0:4, ')');

  r := Tanh(0.0);
  write('START: Tanh(0): ');
  if abs(0.0 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 0.0:0:4, ', got: ', r:0:4, ')');

  r := Max(2.0, 1.0);
  write('START: Max(2,1): ');
  if abs(2.0 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 2.0:0:4, ', got: ', r:0:4, ')');

  r := Min(2.0, 1.0);
  write('START: Min(2,1): ');
  if abs(1.0 - r) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', 1.0:0:4, ', got: ', r:0:4, ')');

  i := Floor(3.7);
  write('START: Floor(3.7): ');
  if 3 = i then
    writeln('PASS')
  else
    writeln('FAIL (expected: 3, got: ', i, ')');

  i := Ceil(3.1);
  write('START: Ceil(3.1): ');
  if 4 = i then
    writeln('PASS')
  else
    writeln('FAIL (expected: 4, got: ', i, ')');
end;

begin
  writeln('Running MathLib tests');
  TestMathLib;
  writeln;
  writeln('MathLib tests completed.');
end.
