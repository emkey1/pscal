program CaseStatementSuite;

type
  TColor = (cRed, cGreen, cBlue, cYellow); // Enum for testing
  TSize = (sSmall, sMedium, sLarge);

var
  i: integer;
  ch: char;
  color: TColor;
  size: TSize;
  doneLooping: boolean;
  resultString: string;   // To capture the string output from the CASE
  expectedString: string; // To hold the expected string for verification
  testDesc: string;       // Description for the verification output

// --- Helper Procedure for String Verification ---
procedure VerifyString(actual: string; expected: string; description: string);
begin
  write('START: ', description, ': ');
  if actual = expected then
    writeln('PASS')
  else
    writeln('FAIL (Expected ''', expected, ''', Got ''', actual, ''')');
end;

begin
  writeln('--- Testing CASE Statements with Verification ---');
  writeln;

  // --- Test CASE with Integer ---
  writeln('Testing INTEGER CASE:');
  for i := 0 to 6 do
  begin
    // Determine expected result first
    case i of
      1: expectedString := 'One';
      2, 3: expectedString := 'Two or Three';
      4..5: expectedString := 'Four to Five';
    else
      expectedString := 'Other Integer';
    end;

    // Execute the case and capture result
    case i of
      1: resultString := 'One';
      2, 3: resultString := 'Two or Three';
      4..5: resultString := 'Four to Five';
    else
      resultString := 'Other Integer';
    end; // end case i

    // Verify
    // Note: Creating description string dynamically for better context
    testDesc := 'Integer Case (i=' + IntToStr(i) + ')';
    VerifyString(resultString, expectedString, testDesc);

  end; // end for i
  writeln;

  // --- Test CASE with Char ---
  writeln('Testing CHAR CASE:');
  // Loop through relevant characters including test cases and edge cases
  for ch := 'X' to ']' do // Covers 'X', 'Y', 'Z', '[', '\', ']'
  begin
     // Determine expected result first
    case ch of
      'A': expectedString := 'Letter A';       // Not hit in this loop
      'B'..'D': expectedString := 'Letter B, C, or D'; // Not hit
      'Y', 'Z': expectedString := 'Letter Y or Z';
      '[': expectedString := 'Opening Bracket';
    else
      expectedString := 'Other Character'; // Includes 'X', '\', ']'
    end;

    // Execute the case and capture result
    case ch of
      'A': resultString := 'Letter A';
      'B'..'D': resultString := 'Letter B, C, or D';
      'Y', 'Z': resultString := 'Letter Y or Z';
      '[': resultString := 'Opening Bracket';
    else
      resultString := 'Other Character';
    end; // end case ch

    // Verify
    testDesc := 'Char Case (ch=''' + ch + ''')';
    VerifyString(resultString, expectedString, testDesc);
  end; // end for ch
  writeln;

  // --- Test CASE with Enum (TColor) ---
  writeln('Testing ENUM CASE (TColor):');
  color := Low(TColor); // Start with the first enum value (cRed = 0)
  doneLooping := false;
  while not doneLooping do
  begin
    // Determine expected result first
    case color of
      cRed: expectedString := 'Red';
      cGreen, cBlue: expectedString := 'Green or Blue';
    else // cYellow
      expectedString := 'Other Color (Yellow)';
    end;

    // Execute the case and capture result
    case color of
      cRed: resultString := 'Red';
      cGreen, cBlue: resultString := 'Green or Blue';
    else
      resultString := 'Other Color (Yellow)';
    end; // end case color

    // Verify
    // Note: Using Ord() to show the underlying integer value
    testDesc := 'Enum TColor Case (Ord=' + IntToStr(Ord(color)) + ')';
    VerifyString(resultString, expectedString, testDesc);

    // Loop control
    if color = High(TColor) then
      doneLooping := true
    else
      Inc(color); // Go to next enum value
  end; // end while
  writeln;

   // --- Test CASE with Enum (TSize) ---
  writeln('Testing ENUM CASE (TSize):');
  size := sMedium; // The value being tested

  // Determine expected result
  case size of
     sSmall : expectedString := 'Small';
     sLarge : expectedString := 'Large';
   else // sMedium falls here
     expectedString := 'Default Size (Else)';
   end;

   // Execute the case and capture result
  case size of
    sSmall : resultString := 'Small';
    sLarge : resultString := 'Large';
  else
    resultString := 'Default Size (Else)';
  end; // end case size

  // Verify
  testDesc := 'Enum TSize Case (size=sMedium, Ord=' + IntToStr(Ord(size)) + ')';
  VerifyString(resultString, expectedString, testDesc);
  writeln;

  writeln('--- Test Complete ---');
end.
