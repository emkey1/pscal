program PointerTortureTest;

const
  TOLERANCE = 0.0001; // For potential future real pointer tests

type
  PInt = ^Integer; // Pointer to Integer
  PStr = ^String;  // Pointer to String
  PRec = ^TRec;   // Pointer to Record
  TRec = record
    id: Integer;
    label: String[50];
    value: Real;
  end;

var
  // Integer pointers
  intPtr1, intPtr2, intPtrNil, intPtrTemp : PInt;

  // String pointers
  strPtr1, strPtr2 : PStr;

  // Record pointers
  recPtrA, recPtrB, recPtrNil : PRec;

  // Auxiliary variables
  i: Integer;
  tempStr: String;
  tempInt: Integer;

{---------------------------------------------------------------------
  Assertion Procedures (Copied for self-containment)
---------------------------------------------------------------------}

procedure AssertTrue(condition: boolean; testName: string);
begin
  write('START: ', testName, ': ');
  if condition then
    writeln('PASS')
  else
    writeln('FAIL (condition was false)');
end;

procedure AssertFalse(condition: boolean; testName: string);
begin
  write('START: ', testName, ': ');
  if not condition then
    writeln('PASS')
  else
    writeln('FAIL (condition was true)');
end;

procedure AssertEqualInt(expected, actual: integer; testName: string);
begin
  write('START: ', testName, ': ');
  if expected = actual then
    writeln('PASS')
  else
    writeln('FAIL (expected: ', expected, ', got: ', actual, ')');
end;

procedure AssertEqualStr(expected, actual: string; testName: string);
begin
  write('START: ', testName, ': ');
  if expected = actual then
    writeln('PASS')
  else
    writeln('FAIL (expected: "', expected, '", got: "', actual, '")');
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
  Pointer Torture Tests
---------------------------------------------------------------------}

procedure TestIntegerPointers;
begin
  writeln;
  writeln('--- Testing Integer Pointers ---');

  // 1. Initialization and Nil Checks
  writeln('1. Initialization and Nil Checks...');
  intPtr1 := nil;
  intPtr2 := nil;
  intPtrNil := nil;
  AssertTrue(intPtr1 = nil, 'intPtr1 initialized to nil');
  AssertTrue(intPtr2 = nil, 'intPtr2 initialized to nil');
  AssertTrue(intPtr1 = intPtrNil, 'intPtr1 = intPtrNil (nil comparison)');
  AssertFalse(intPtr1 <> nil, 'intPtr1 <> nil (should be false)');

  // 2. Allocation with new()
  writeln('2. Allocation with new()...');
  new(intPtr1);
  AssertFalse(intPtr1 = nil, 'intPtr1 is not nil after new()');
  // Note: Value pointed to by intPtr1 is undefined here.

  // 3. Assignment via Dereference
  writeln('3. Assignment via Dereference...');
  intPtr1^ := 12345;
  AssertEqualInt(12345, intPtr1^, 'Assign and read intPtr1^');

  // 4. Pointer Assignment (Aliasing)
  writeln('4. Pointer Assignment (Aliasing)...');
  intPtr2 := intPtr1; // intPtr2 now points to the SAME memory as intPtr1
  AssertFalse(intPtr2 = nil, 'intPtr2 is not nil after assignment');
  AssertTrue(intPtr1 = intPtr2, 'intPtr1 = intPtr2 (same address)');
  AssertEqualInt(12345, intPtr2^, 'Read intPtr2^ after aliasing');

  // 5. Modifying through one alias affects the other
  writeln('5. Modifying through alias...');
  intPtr2^ := 54321;
  AssertEqualInt(54321, intPtr2^, 'Read intPtr2^ after modification');
  AssertEqualInt(54321, intPtr1^, 'Read intPtr1^ after modification via intPtr2');
  intPtr1^ := 789;
  AssertEqualInt(789, intPtr1^, 'Read intPtr1^ after modification');
  AssertEqualInt(789, intPtr2^, 'Read intPtr2^ after modification via intPtr1');

  // 6. Disposal
  writeln('6. Disposal...');
  dispose(intPtr1);
  AssertTrue(intPtr1 = nil, 'intPtr1 is nil after dispose(intPtr1)');
  // Accessing intPtr1^ or intPtr2^ now leads to runtime error or undefined behavior.
  // Standard Pascal does not require intPtr2 to become nil here.
  // Our implementation might or might not set aliases to nil upon dispose,
  // but the memory they point to is invalid.

  // 7. Disposing nil pointer (should be safe)
  writeln('7. Disposing nil pointer...');
  AssertTrue(intPtrNil = nil, 'intPtrNil is nil before dispose');
  dispose(intPtrNil);
  AssertTrue(intPtrNil = nil, 'intPtrNil is still nil after dispose(nil)'); // Should not change

  // 8. Disposing an already disposed pointer (via alias)
  // Standard Pascal: Error. Our implementation: Check behavior (might crash or do nothing)
  // We expect intPtr2 still holds the old (now invalid) address unless dispose(p1) also nils aliases.
  // Let's try disposing it - this is implementation-dependent.
  writeln('8. Disposing already disposed memory (via alias)...');
  // Assuming dispose(p1) did NOT nil p2:
  if intPtr2 <> nil then
  begin
     writeln('   Attempting dispose(intPtr2) which points to freed memory...');
     dispose(intPtr2); // Might cause runtime error, or might "work" and set intPtr2 to nil
     AssertTrue(intPtr2 = nil, 'intPtr2 is nil after disposing freed memory');
  end
  else
  begin
      writeln('   Skipping dispose(intPtr2) because it was already nil (implies dispose(p1) affects aliases).');
  end;

  // 9. Re-allocating a disposed pointer
  writeln('9. Re-allocating disposed pointer...');
  new(intPtr1); // Should work fine, gets new memory
  AssertFalse(intPtr1 = nil, 'intPtr1 is not nil after re-allocation');
  intPtr1^ := 111;
  AssertEqualInt(111, intPtr1^, 'Assign/read after re-allocation');
  dispose(intPtr1);
  AssertTrue(intPtr1 = nil, 'intPtr1 is nil after final dispose');

end;

procedure TestStringPointers;
begin
  writeln;
  writeln('--- Testing String Pointers ---');

  strPtr1 := nil;
  AssertTrue(strPtr1 = nil, 'strPtr1 initialized to nil');

  new(strPtr1);
  AssertFalse(strPtr1 = nil, 'strPtr1 not nil after new()');
  // Value is initially undefined (or empty string depending on makeValueForType)
  // Let's assume makeValueForType initializes string to ""
  AssertEqualStr('', strPtr1^, 'strPtr1^ initial value');

  strPtr1^ := 'Hello';
  AssertEqualStr('Hello', strPtr1^, 'Assign/read strPtr1^');

  strPtr2 := strPtr1; // Aliasing
  strPtr2^ := strPtr2^ + ', World!'; // Modify through alias
  AssertEqualStr('Hello, World!', strPtr1^, 'Read strPtr1^ after modification via strPtr2');

  dispose(strPtr1);
  AssertTrue(strPtr1 = nil, 'strPtr1 is nil after dispose');
  // strPtr2 now dangles

end;

procedure TestRecordPointers;
begin
  writeln;
  writeln('--- Testing Record Pointers ---');

  // 1. Initialization and Nil Check
  recPtrA := nil;
  recPtrB := nil;
  recPtrNil := nil;
  AssertTrue(recPtrA = nil, 'recPtrA initialized to nil');
  AssertTrue(recPtrA = recPtrNil, 'recPtrA = recPtrNil');

  // 2. Allocation
  writeln('2. Allocation...');
  new(recPtrA);
  AssertFalse(recPtrA = nil, 'recPtrA not nil after new()');
  // Fields are initialized by makeValueForType/createEmptyRecord
  AssertEqualInt(0, recPtrA^.id, 'Initial recPtrA^.id'); // Assuming integer initializes to 0
  AssertEqualStr('', recPtrA^.label, 'Initial recPtrA^.label'); // Assuming string initializes to ''
  AssertEqualReal(0.0, recPtrA^.value, 'Initial recPtrA^.value'); // Assuming real initializes to 0.0

  // 3. Assigning values to fields
  writeln('3. Assigning field values...');
  recPtrA^.id := 101;
  recPtrA^.label := 'First Record';
  recPtrA^.value := 3.14;
  AssertEqualInt(101, recPtrA^.id, 'Read recPtrA^.id after assignment');
  AssertEqualStr('First Record', recPtrA^.label, 'Read recPtrA^.label after assignment');
  AssertEqualReal(3.14, recPtrA^.value, 'Read recPtrA^.value after assignment');

  // 4. Pointer Assignment (Aliasing)
  writeln('4. Pointer Assignment (Aliasing)...');
  recPtrB := recPtrA;
  AssertTrue(recPtrA = recPtrB, 'recPtrA = recPtrB');
  AssertEqualInt(101, recPtrB^.id, 'Read recPtrB^.id via alias');

  // 5. Modifying via alias
  writeln('5. Modifying via alias...');
  recPtrB^.id := 202;
  recPtrB^.label := 'Modified Label';
  AssertEqualInt(202, recPtrA^.id, 'Read recPtrA^.id after modification via B');
  AssertEqualStr('Modified Label', recPtrA^.label, 'Read recPtrA^.label after modification via B');

  // 6. Disposal
  writeln('6. Disposal...');
  dispose(recPtrA);
  AssertTrue(recPtrA = nil, 'recPtrA is nil after dispose');
  // recPtrB now dangles

  // 7. Disposing nil
  dispose(recPtrNil);
  AssertTrue(recPtrNil = nil, 'recPtrNil is still nil after dispose(nil)');

end;


{---------------------------------------------------------------------
  Main Program
---------------------------------------------------------------------}
begin
  writeln('Running pscal Pointer Torture Test');

  TestIntegerPointers;
  TestStringPointers;
  TestRecordPointers;

  writeln;
  writeln('Pointer Torture Test Completed.');
end.
