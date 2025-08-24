program TestArrayConst;
uses crt; // Optional, for clrscr if desired

const
  // String array constant (like your example)
  CardValues: array[1..13] of string = (
    'A', '2', '3', '4', '5', '6', '7', '8', '9', '10', 'J', 'Q', 'K'
  );

  // Integer array constant
  IntSequence: array[0..4] of integer = (10, -5, 100, 0, 42);

  // Simple constant for comparison
  TestStr: string = 'Simple';

var
  i: integer;
  s: string;

begin
  clrscr; // Optional clear screen

  writeln('Testing Typed Array Constants:');
  writeln('------------------------------');

  // Test simple constant (ensure it still works)
  writeln('Simple Const TestStr: ', TestStr);
  writeln;

  // Test string array constant access
  writeln('Testing CardValues (String Array):');
  writeln('CardValues[1] = ', CardValues[1], '(A)');   // Expected: A
  writeln('CardValues[10] = ', CardValues[10], '(10)'); // Expected: 10
  writeln('CardValues[13] = ', CardValues[13], '(K)'); // Expected: K

  // Assign an element to a variable
  s := CardValues[11];
  writeln('Assigned CardValues[11] to s: ', s); // Expected: J
  writeln;

  // Test integer array constant access
  writeln('Testing IntSequence (Integer Array):');
  writeln('IntSequence[0] = ', IntSequence[0], '(10)'); // Expected: 10
  writeln('IntSequence[1] = ', IntSequence[1], '(-5)'); // Expected: -5
  writeln('IntSequence[4] = ', IntSequence[4], '(42)'); // Expected: 42

  // Loop through and print integer array
  writeln('Looping through IntSequence:');
  // Note: Using hardcoded bounds as low()/high() on constants might not be implemented
  for i := 0 to 4 do
  begin
    write(IntSequence[i], ' ');
  end;
  writeln; // Newline after loop

  writeln;
  writeln('Test complete.');
end.
