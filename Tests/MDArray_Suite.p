program MultiDimArrayTest;

const
  TOLERANCE = 0.0001; // For real number comparisons

type
  Matrix = array[1..3, 0..2] of integer; // 2D Array (3 rows, 3 columns with 0-based index)
  Cube   = array[-1..0, 1..2, 3..4] of real;    // 3D Array (2x2x2 elements, non-zero based indices)

var
  matrix_a: Matrix;
  cube_a: Cube;
  i, j, k: integer;
  r_val: real;
  i_val: integer;
  checksum_i: integer;
  checksum_r: real;

{---------------------------------------------------------------------
  Assertion Procedures (Copied for self-containment)
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
begin
  write('START: ', testName, ': ');
  if abs(expected - actual) < TOLERANCE then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', expected:0:4, ', got: ', actual:0:4, ')');
end;

{---------------------------------------------------------------------
  Multi-Dimensional Array Tests
---------------------------------------------------------------------}

procedure Test2DArrayAssignmentAndAccess;
begin
  writeln;
  writeln('--- Testing 2D Array (Matrix) ---');

  // Assign values using nested loops
  writeln('Assigning values to matrix_a[1..3, 0..2]...');
  checksum_i := 0;
  for i := 1 to 3 do // Iterate through first dimension (rows)
  begin
    for j := 0 to 2 do // Iterate through second dimension (columns)
    begin
      matrix_a[i, j] := i * 10 + j; // Assign value based on indices
      checksum_i := checksum_i + matrix_a[i, j]; // Add to checksum for later verification
      // writeln('DEBUG: Assigned matrix_a[', i, ',', j, '] = ', matrix_a[i, j]);
    end;
  end;
  writeln('Assignment complete.');
  AssertEqualInt(189, checksum_i, '2D Array Checksum after assignment'); // (10+11+12) + (20+21+22) + (30+31+32) = 33 + 63 + 93 = 189. Hmm, let's recheck. 1*10+0=10, 1*10+1=11, 1*10+2=12. 2*10+0=20, 2*10+1=21, 2*10+2=22. 3*10+0=30, 3*10+1=31, 3*10+2=32. Sum=10+11+12+20+21+22+30+31+32 = 33+63+93 = 189. Correcting assertion.
  AssertEqualInt(189, checksum_i, '2D Array Checksum after assignment');


  // Verify specific elements, including boundaries
  writeln('Verifying individual elements...');
  AssertEqualInt(10, matrix_a[1, 0], '2D Access matrix_a[1, 0]');
  AssertEqualInt(12, matrix_a[1, 2], '2D Access matrix_a[1, 2] (Edge)');
  AssertEqualInt(21, matrix_a[2, 1], '2D Access matrix_a[2, 1]');
  AssertEqualInt(30, matrix_a[3, 0], '2D Access matrix_a[3, 0] (Edge)');
  AssertEqualInt(32, matrix_a[3, 2], '2D Access matrix_a[3, 2] (Corner)');

  // Modify an element and verify
  matrix_a[2, 1] := 999;
  AssertEqualInt(999, matrix_a[2, 1], '2D Modify/Access matrix_a[2, 1]');
end;

procedure Test3DArrayAssignmentAndAccess;
begin
  writeln;
  writeln('--- Testing 3D Array (Cube) ---');

  // Assign values using nested loops
  writeln('Assigning values to cube_a[-1..0, 1..2, 3..4]...');
  checksum_r := 0.0;
  for i := -1 to 0 do // Dimension 1
  begin
    for j := 1 to 2 do // Dimension 2
    begin
      for k := 3 to 4 do // Dimension 3
      begin
        // Assign value based on indices (using real numbers)
        cube_a[i, j, k] := (i * 100.0) + (j * 10.0) + k;
        checksum_r := checksum_r + cube_a[i, j, k]; // Add to checksum
        // writeln('DEBUG: Assigned cube_a[', i, ',', j, ',', k, '] = ', cube_a[i, j, k]:0:2);
      end;
    end;
  end;
  writeln('Assignment complete.');
  // Checksum: i=-1: (-100+10+3) + (-100+10+4) + (-100+20+3) + (-100+20+4) = -87 + -86 + -77 + -76 = -326
  // Checksum: i=0 : (0+10+3) + (0+10+4) + (0+20+3) + (0+20+4) = 13 + 14 + 23 + 24 = 74
  // Total = -326 + 74 = -252
  AssertEqualReal(-252.0, checksum_r, '3D Array Checksum after assignment');


  // Verify specific elements, including boundaries
  writeln('Verifying individual elements...');
  AssertEqualReal(-87.0, cube_a[-1, 1, 3], '3D Access cube_a[-1, 1, 3] (Corner)');
  AssertEqualReal(-76.0, cube_a[-1, 2, 4], '3D Access cube_a[-1, 2, 4] (Edge)');
  AssertEqualReal(13.0, cube_a[0, 1, 3], '3D Access cube_a[0, 1, 3] (Edge)');
  AssertEqualReal(24.0, cube_a[0, 2, 4], '3D Access cube_a[0, 2, 4] (Corner)');

  // Modify an element and verify
  cube_a[0, 1, 3] := 9.87;
  AssertEqualReal(9.87, cube_a[0, 1, 3], '3D Modify/Access cube_a[0, 1, 3]');
end;


{---------------------------------------------------------------------
  Main Program
---------------------------------------------------------------------}
begin
  writeln('Running pscal Multi-Dimensional Array Torture Test');

  Test2DArrayAssignmentAndAccess;
  Test3DArrayAssignmentAndAccess;

  writeln;
  writeln('Multi-Dimensional Array Torture Test Completed.');
end.
