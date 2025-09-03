program MathLibTest;

uses
  MathLib;

const
  TOL = 0.0001;

procedure AssertEqualInt(expected, actual: integer; testName: string);
begin
  write('START: ', testName, ': ');
  if expected = actual then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', expected, ', got: ', actual, ')');
end;

procedure AssertEqualReal(expected, actual: real; testName: string);
begin
  write('START: ', testName, ': ');
  if abs(expected - actual) < TOL then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', expected:0:4, ', got: ', actual:0:4, ')');
end;

procedure TestMathLib;
begin
  writeln;
  writeln('--- Testing MathLib ---');
  AssertEqualReal(0.7854, ArcTan(1.0), 'ArcTan(1)');
  AssertEqualReal(0.5236, ArcSin(0.5), 'ArcSin(0.5)');
  AssertEqualReal(1.0472, ArcCos(0.5), 'ArcCos(0.5)');
  AssertEqualReal(1.0, Cotan(0.785398), 'Cotan(pi/4)');
  AssertEqualReal(8.0, Power(2.0, 3.0), 'Power(2,3)');
  AssertEqualReal(3.0, Log10(1000.0), 'Log10(1000)');
  AssertEqualReal(0.0, Sinh(0.0), 'Sinh(0)');
  AssertEqualReal(1.0, Cosh(0.0), 'Cosh(0)');
  AssertEqualReal(0.0, Tanh(0.0), 'Tanh(0)');
  AssertEqualReal(2.0, Max(2.0, 1.0), 'Max(2,1)');
  AssertEqualReal(1.0, Min(2.0, 1.0), 'Min(2,1)');
  AssertEqualInt(3, Floor(3.7), 'Floor(3.7)');
  AssertEqualInt(4, Ceil(3.1), 'Ceil(3.1)');
end;

begin
  writeln('Running MathLib tests');
  TestMathLib;
  writeln;
  writeln('MathLib tests completed.');
end.
