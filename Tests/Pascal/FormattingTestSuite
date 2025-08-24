PROGRAM FormattingTestSuite;

USES Crt; // Assumes Crt provides Write, WriteLn, ClrScr

VAR
  i_pos, i_neg, i_zero : Integer;
  r_pos, r_neg, r_zero : Real;
  s_short, s_long    : String; // Use default dynamic strings
  c_val              : Char;
  b_true, b_false    : Boolean;

{---------------------------------------------------------------------
  Test Procedures
---------------------------------------------------------------------}

procedure TestIntegerFormatting;
begin
  writeln;
  writeln('--- Testing Integer Formatting (:width) ---');
  i_pos := 123;
  i_neg := -45;
  i_zero := 0;

  // Positive Integers
  writeln('Positive Integer Tests:');
  write('Val=', i_pos, '<');                        // Expected: Val=123<
  write(' :5 >', i_pos:5, '<');                     // Expected:  :5 >  123<  (Right-justified, padded)
  write(' :3 >', i_pos:3, '<');                     // Expected:  :3 >123<   (Exact width)
  write(' :1 >', i_pos:1, '<');                     // Expected:  :1 >123<   (Width too small, prints full number)
  writeln; // Newline

  // Negative Integers
  writeln('Negative Integer Tests:');
  write('Val=', i_neg, '<');                       // Expected: Val=-45<
  write(' :5 >', i_neg:5, '<');                    // Expected:  :5 >  -45< (Includes sign in width)
  write(' :3 >', i_neg:3, '<');                    // Expected:  :3 >-45<  (Exact width)
  write(' :1 >', i_neg:1, '<');                    // Expected:  :1 >-45<  (Width too small)
  writeln; // Newline

  // Zero
  writeln('Zero Integer Tests:');
  write('Val=', i_zero, '<');                      // Expected: Val=0<
  write(' :5 >', i_zero:5, '<');                   // Expected:  :5 >    0<
  write(' :1 >', i_zero:1, '<');                   // Expected:  :1 >0<
  writeln; // Newline

end;

procedure TestRealFormatting;
begin
  writeln;
  writeln('--- Testing Real Formatting (:width, :width:decimals) ---');
  r_pos := 123.4567;
  r_neg := -98.76;
  r_zero := 0.0;

  // Positive Reals
  writeln('Positive Real Tests:');
  write('Val=', r_pos:0:4, '<');                  // Expected: Val=123.4567< (Default width)
  write(' :10:2 >', r_pos:10:2, '<');             // Expected:  :10:2 >    123.46< (Padded, rounded)
  write(' :7:1 >', r_pos:7:1, '<');               // Expected:  :7:1 >  123.5< (Padded, rounded)
  write(' :5:0 >', r_pos:5:0, '<');               // Expected:  :5:0 >  123< (Padded, rounded to integer)
  write(' :10 >', r_pos:10, '<');                 // Expected:  :10 > 1.23E+02< (Default decimals, scientific/float format, padded) - *Actual format may vary*
  writeln;

  // Negative Reals
  writeln('Negative Real Tests:');
  write('Val=', r_neg:0:2, '<');                  // Expected: Val=-98.76<
  write(' :8:1 >', r_neg:8:1, '<');               // Expected:  :8:1 >   -98.8< (Padded, rounded)
  write(' :5:0 >', r_neg:5:0, '<');               // Expected:  :5:0 >  -99< (Padded, rounded)
  write(' :12 >', r_neg:12, '<');                 // Expected:  :12 >  -9.88E+01< (Default decimals, scientific/float format, padded) - *Actual format may vary*
  writeln;

  // Zero Real
  writeln('Zero Real Tests:');
  write('Val=', r_zero:0:1, '<');                 // Expected: Val=0.0<
  write(' :8:2 >', r_zero:8:2, '<');              // Expected:  :8:2 >    0.00<
  write(' :5 >', r_zero:5, '<');                  // Expected:  :5 > 0.0E+00< (Default decimals) - *Actual format may vary*
  writeln;

end;

procedure TestStringFormatting;
begin
  writeln;
  writeln('--- Testing String Formatting (:width) ---');
  s_short := 'Hi';
  s_long := 'Pascal';

  // Short String
  writeln('Short String Tests:');
  write('Val=', s_short, '<');                     // Expected: Val=Hi<
  write(' :5 >', s_short:5, '<');                  // Expected:  :5 >   Hi< (Right-justified, padded left)
  write(' :2 >', s_short:2, '<');                  // Expected:  :2 >Hi<   (Exact width)
  write(' :1 >', s_short:1, '<');                  // Expected:  :1 >H<    (Truncated right in standard Pascal) *Interpreter might differ*
  writeln;

  // Longer String
  writeln('Longer String Tests:');
  write('Val=', s_long, '<');                      // Expected: Val=Pascal<
  write(' :10 >', s_long:10, '<');                 // Expected:  :10 >    Pascal<
  write(' :6 >', s_long:6, '<');                   // Expected:  :6 >Pascal<
  write(' :4 >', s_long:4, '<');                   // Expected:  :4 >Pasc< (Truncated right in standard Pascal) *Interpreter might differ*
  writeln;
end;

procedure TestOtherFormatting;
begin
  writeln;
  writeln('--- Testing Other Types (Behavior May Vary) ---');
  c_val := 'X';
  b_true := True;
  b_false := False;

  // Char (Standard Pascal typically doesn't pad char with :width)
  write('Char=', c_val, '<');                      // Expected: Char=X<
  write(' :5 >', c_val:5, '<');                    // Expected:  :5 >X< (Or maybe '    X', depending on implementation)
  writeln;

  // Boolean (Standard Pascal typically doesn't pad boolean with :width)
  write('Bool=', b_true, '<');                     // Expected: Bool=TRUE<
  write(' :8 >', b_true:8, '<');                   // Expected:  :8 >TRUE< (Or maybe '    TRUE', depending on implementation)
  write(' :8 >', b_false:8, '<');                  // Expected:  :8 >FALSE< (Or maybe '   FALSE', depending on implementation)
  writeln;
end;

{---------------------------------------------------------------------
  Main Program
---------------------------------------------------------------------}
begin
  ClrScr;
  writeln('Running pscal Formatting Test Suite');
  writeln('===================================');
  writeln('(Check output visually against comments in source code)');

  TestIntegerFormatting;
  TestRealFormatting;
  TestStringFormatting;
  TestOtherFormatting;

  writeln;
  writeln('Formatting Test Suite Completed.');
end.
