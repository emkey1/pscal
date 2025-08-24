program MathTortureTest;

const // <-- Move TOLERANCE declaration here
  TOLERANCE = 0.0001; // Define a small tolerance for real comparisons

var
  i1, i2, i3, i_res: integer;
  r1, r2, r3, r_res: real;
  pi_approx: real;

{---------------------------------------------------------------------
  Assertion Procedures
---------------------------------------------------------------------}

procedure AssertEqualInt(expected, actual: integer; testName: string);
begin
  write('START: ', testName, ': ');
  if expected = actual then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', expected, ', got: ', actual, ')');
end;

procedure AssertEqualReal(expected, actual: real; testName: string);
// No const declaration needed inside the procedure anymore
begin
  write('START: ', testName, ': ');
  // Use the globally defined TOLERANCE
  if abs(expected - actual) < TOLERANCE then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', expected:0:4, ', got: ', actual:0:4, ')');
end;

{---------------------------------------------------------------------
  Math Test Procedures (Keep these as they were)
---------------------------------------------------------------------}

procedure TestIntegerArithmetic;
begin
  writeln;
  writeln('--- Testing Integer Arithmetic ---');
  i1 := 10;
  i2 := 3;
  i3 := -5;

  AssertEqualInt(13, i1 + i2, 'Int Add (10 + 3)');
  AssertEqualInt(7, i1 - i2, 'Int Subtract (10 - 3)');
  AssertEqualInt(30, i1 * i2, 'Int Multiply (10 * 3)');
  AssertEqualInt(3, i1 div i2, 'Int Div (10 div 3)');
  AssertEqualInt(1, i1 mod i2, 'Int Mod (10 mod 3)');

  AssertEqualInt(5, i1 + i3, 'Int Add Negative (10 + -5)');
  AssertEqualInt(15, i1 - i3, 'Int Subtract Negative (10 - -5)');
  AssertEqualInt(-50, i1 * i3, 'Int Multiply Negative (10 * -5)');
  AssertEqualInt(-2, i1 div i3, 'Int Div Negative (10 div -5)');
  AssertEqualInt(0, i1 mod i3, 'Int Mod Negative (10 mod -5)');

  AssertEqualInt(-8, i3 - i2, 'Int Neg Subtract Pos (-5 - 3)');
  AssertEqualInt(-15, i3 * i2, 'Int Neg Multiply Pos (-5 * 3)');
  AssertEqualInt(-1, i3 div i2, 'Int Neg Div Pos (-5 div 3)');
  AssertEqualInt(-2, i3 mod i2, 'Int Neg Mod Pos (-5 mod 3)');
end;

procedure TestRealArithmetic;
begin
  writeln;
  writeln('--- Testing Real Arithmetic ---');
  r1 := 10.5;
  r2 := 2.0;
  r3 := -4.25;
  pi_approx := 3.14159; // Approximation for trig functions

  AssertEqualReal(12.5, r1 + r2, 'Real Add (10.5 + 2.0)');
  AssertEqualReal(8.5, r1 - r2, 'Real Subtract (10.5 - 2.0)');
  AssertEqualReal(21.0, r1 * r2, 'Real Multiply (10.5 * 2.0)');
  AssertEqualReal(5.25, r1 / r2, 'Real Divide (10.5 / 2.0)');

  AssertEqualReal(6.25, r1 + r3, 'Real Add Negative (10.5 + -4.25)');
  AssertEqualReal(14.75, r1 - r3, 'Real Subtract Negative (10.5 - -4.25)');
  AssertEqualReal(-44.625, r1 * r3, 'Real Multiply Negative (10.5 * -4.25)');
  AssertEqualReal(-2.4706, r1 / r3, 'Real Divide Negative (10.5 / -4.25)');

  AssertEqualReal(-6.25, r3 - r2, 'Real Neg Subtract Pos (-4.25 - 2.0)');
  AssertEqualReal(-8.5, r3 * r2, 'Real Neg Multiply Pos (-4.25 * 2.0)');
  AssertEqualReal(-2.125, r3 / r2, 'Real Neg Divide Pos (-4.25 / 2.0)');
end;

procedure TestMixedArithmetic;
begin
  writeln;
  writeln('--- Testing Mixed Integer/Real Arithmetic ---');
  i1 := 10;
  r1 := 2.5;

  AssertEqualReal(12.5, i1 + r1, 'Mixed Add (int + real)');
  AssertEqualReal(12.5, r1 + i1, 'Mixed Add (real + int)');
  AssertEqualReal(7.5, i1 - r1, 'Mixed Subtract (int - real)');
  AssertEqualReal(-7.5, r1 - i1, 'Mixed Subtract (real - int)');
  AssertEqualReal(25.0, i1 * r1, 'Mixed Multiply (int * real)');
  AssertEqualReal(25.0, r1 * i1, 'Mixed Multiply (real * int)');
  AssertEqualReal(4.0, i1 / r1, 'Mixed Divide / (int / real)');
  AssertEqualReal(0.25, r1 / i1, 'Mixed Divide / (real / int)');
end;

procedure TestOperatorPrecedenceTorture;
begin
  writeln;
  writeln('--- Testing Operator Precedence ---');
  i1 := 2; i2 := 3; i3 := 4;
  r1 := 10.0; r2 := 2.0; r3 := 5.0;

  AssertEqualInt(14, i1 + i2 * i3, 'Precedence (2 + 3 * 4)');
  AssertEqualInt(20, (i1 + i2) * i3, 'Precedence ((2 + 3) * 4)');
  AssertEqualInt(9, i3 * i1 + i2 - i1, 'Precedence (4*2 + 3 - 2)');

  AssertEqualReal(25.0, r1 / r2 * r3, 'Precedence (10.0 / 2.0 * 5.0)');
  AssertEqualReal(1.0, r1 / (r2 * r3), 'Precedence (10.0 / (2.0 * 5.0))');
  AssertEqualReal(12.0, r1 + r3 - r2 * 1.5, 'Precedence (10+5 - 2*1.5)');
end;

procedure TestBuiltinMathFunctions;
begin
  writeln;
  writeln('--- Testing Built-in Math Functions ---');
  pi_approx := 3.14159;

  AssertEqualInt(5, abs(-5), 'Abs (int -5)');
  AssertEqualInt(5, abs(5), 'Abs (int 5)');
  AssertEqualReal(3.14, abs(-3.14), 'Abs (real -3.14)');
  AssertEqualReal(3.14, abs(3.14), 'Abs (real 3.14)');
  AssertEqualReal(0.0, abs(0.0), 'Abs (real 0.0)');

  AssertEqualReal(2.0, sqrt(4), 'Sqrt (int 4)');
  AssertEqualReal(1.4142, sqrt(2.0), 'Sqrt (real 2.0)');
  AssertEqualReal(0.0, sqrt(0.0), 'Sqrt (real 0.0)');

  AssertEqualInt(3, trunc(3.7), 'Trunc (3.7)');
  AssertEqualInt(-3, trunc(-3.7), 'Trunc (-3.7)');
  AssertEqualInt(5, trunc(5.0), 'Trunc (5.0)');
  AssertEqualInt(0, trunc(0.1), 'Trunc (0.1)');
  AssertEqualInt(0, trunc(-0.1), 'Trunc (-0.1)');

  AssertEqualReal(1.0, exp(0.0), 'Exp (0.0)');
  AssertEqualReal(2.7182, exp(1.0), 'Exp (1.0)');
  AssertEqualReal(0.3678, exp(-1.0), 'Exp (-1.0)');
  AssertEqualReal(7.3890, exp(2), 'Exp (int 2)');

  AssertEqualReal(0.0, ln(1.0), 'Ln (1.0)');
  AssertEqualReal(1.0, ln(exp(1.0)), 'Ln (exp(1.0))');
  AssertEqualReal(2.3025, ln(10.0), 'Ln (10.0)');

  AssertEqualReal(1.0, cos(0.0), 'Cos (0.0)');
  AssertEqualReal(-1.0, cos(pi_approx), 'Cos (pi)');
  AssertEqualReal(0.0, cos(pi_approx / 2.0), 'Cos (pi/2)');

  AssertEqualReal(0.0, sin(0.0), 'Sin (0.0)');
  AssertEqualReal(0.0, sin(pi_approx), 'Sin (pi)');
  AssertEqualReal(1.0, sin(pi_approx / 2.0), 'Sin (pi/2)');

  AssertEqualReal(0.0, tan(0.0), 'Tan (0.0)');
  AssertEqualReal(1.0, tan(pi_approx / 4.0), 'Tan (pi/4)');
end;

{---------------------------------------------------------------------
  Main Program
---------------------------------------------------------------------}
begin
  writeln('Running pscal Math Torture Test');

  TestIntegerArithmetic;
  TestRealArithmetic;
  TestMixedArithmetic;
  TestOperatorPrecedenceTorture;
  TestBuiltinMathFunctions;

  writeln;
  writeln('Math Torture Test Completed.');
end.
