program TestIncDecWithVerify;

type
  TMyRecord = record
    count: integer;
    label: string; // Add another field for variety
  end;

var
  myVar: integer;
  myRecord: TMyRecord;
  myArray: array[1..3] of integer;
  step: integer;
  expectedValue: integer; // Variable to hold expected result for verification

// --- Helper Procedure for Verification ---
procedure Verify(actual: integer; expected: integer; description: string);
begin
  write('START: ', description, ': ');
  if actual = expected then
    writeln('PASS')
  else
    writeln('FAIL (Expected ', expected, ', Got ', actual, ')');
end;

begin
  writeln('--- Testing Inc/Dec with Verification ---');
  writeln;

  // --- Test Simple Variable ---
  writeln('Testing Simple Variable (myVar)...');
  myVar := 10;
  writeln('Initial myVar: ', myVar); // Still useful to see the start

  Inc(myVar);
  expectedValue := 11;
  Verify(myVar, expectedValue, 'Simple Inc(myVar)'); // Expected: 11

  Dec(myVar);
  Dec(myVar);
  expectedValue := 9;
  Verify(myVar, expectedValue, 'Simple Dec(myVar)*2'); // Expected: 9

  step := 5;
  Inc(myVar, step);
  expectedValue := 14;
  Verify(myVar, expectedValue, 'Simple Inc(myVar, 5)'); // Expected: 14

  Dec(myVar, 3);
  expectedValue := 11;
  Verify(myVar, expectedValue, 'Simple Dec(myVar, 3)'); // Expected: 11
  writeln; // Add spacing

  // --- Test Record Field ---
  writeln('Testing Record Field (myRecord.count)...');
  myRecord.count := 50;
  myRecord.label := 'Initial';
  writeln('Initial myRecord.count: ', myRecord.count); // Initial value

  Inc(myRecord.count);
  expectedValue := 51;
  Verify(myRecord.count, expectedValue, 'Record Inc(myRecord.count)'); // Expected: 51

  Dec(myRecord.count);
  Dec(myRecord.count);
  expectedValue := 49;
  Verify(myRecord.count, expectedValue, 'Record Dec(myRecord.count)*2'); // Expected: 49

  Inc(myRecord.count, 10);
  expectedValue := 59;
  Verify(myRecord.count, expectedValue, 'Record Inc(myRecord.count, 10)'); // Expected: 59

  Dec(myRecord.count, 4);
  expectedValue := 55;
  Verify(myRecord.count, expectedValue, 'Record Dec(myRecord.count, 4)'); // Expected: 55
  writeln; // Add spacing

  // --- Test Array Element ---
  writeln('Testing Array Element (myArray[2])...');
  myArray[1] := 100;
  myArray[2] := 200;
  myArray[3] := 300;
  writeln('Initial myArray[2]: ', myArray[2]); // Initial value

  Inc(myArray[2]);
  expectedValue := 201;
  Verify(myArray[2], expectedValue, 'Array Inc(myArray[2])'); // Expected: 201

  Dec(myArray[2]);
  Dec(myArray[2]);
  expectedValue := 199;
  Verify(myArray[2], expectedValue, 'Array Dec(myArray[2])*2'); // Expected: 199

  Inc(myArray[2], 20);
  expectedValue := 219;
  Verify(myArray[2], expectedValue, 'Array Inc(myArray[2], 20)'); // Expected: 219

  Dec(myArray[2], 9);
  expectedValue := 210;
  Verify(myArray[2], expectedValue, 'Array Dec(myArray[2], 9)'); // Expected: 210
  writeln; // Add spacing

  // --- Verify other elements unchanged ---
  // We can add checks for these too if needed, using the same Verify procedure
  writeln('Verifying other array elements...');
  Verify(myArray[1], 100, 'Array Unchanged myArray[1]'); // Expected: 100
  Verify(myArray[3], 300, 'Array Unchanged myArray[3]'); // Expected: 300
  writeln;

  writeln('--- Test Complete ---');
end.
