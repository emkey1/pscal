program TestIncDec;

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

begin
  writeln('--- Testing Inc/Dec ---');

  // Test Simple Variable
  myVar := 10;
  writeln('Initial myVar: ', myVar); // Expected: 10
  Inc(myVar);
  writeln('After Inc(myVar): ', myVar); // Expected: 11
  Dec(myVar);
  Dec(myVar);
  writeln('After Dec(myVar)*2: ', myVar); // Expected: 9
  step := 5;
  Inc(myVar, step);
  writeln('After Inc(myVar, 5): ', myVar); // Expected: 14
  Dec(myVar, 3);
  writeln('After Dec(myVar, 3): ', myVar); // Expected: 11
  writeln;

  // Test Record Field
  myRecord.count := 50;
  myRecord.label := 'Initial';
  writeln('Initial myRecord.count: ', myRecord.count); // Expected: 50
  Inc(myRecord.count);
  writeln('After Inc(myRecord.count): ', myRecord.count); // Expected: 51
  Dec(myRecord.count);
  Dec(myRecord.count);
  writeln('After Dec(myRecord.count)*2: ', myRecord.count); // Expected: 49
  Inc(myRecord.count, 10);
  writeln('After Inc(myRecord.count, 10): ', myRecord.count); // Expected: 59
   Dec(myRecord.count, 4);
  writeln('After Dec(myRecord.count, 4): ', myRecord.count); // Expected: 55
  writeln;

  // Test Array Element
  myArray[1] := 100;
  myArray[2] := 200;
  myArray[3] := 300;
  writeln('Initial myArray[2]: ', myArray[2]); // Expected: 200
  Inc(myArray[2]);
  writeln('After Inc(myArray[2]): ', myArray[2]); // Expected: 201
  Dec(myArray[2]);
  Dec(myArray[2]);
  writeln('After Dec(myArray[2])*2: ', myArray[2]); // Expected: 199
  Inc(myArray[2], 20);
  writeln('After Inc(myArray[2], 20): ', myArray[2]); // Expected: 219
  Dec(myArray[2], 9);
  writeln('After Dec(myArray[2], 9): ', myArray[2]); // Expected: 210

  // Verify other elements unchanged
  writeln('Final myArray[1]: ', myArray[1]); // Expected: 100
  writeln('Final myArray[3]: ', myArray[3]); // Expected: 300
  writeln;

  writeln('--- Test Complete ---');
end.
