program TestVarParam;

var
  x, y: integer;

{ Increase takes a variable parameter n (passed by reference)
  and a value delta. Changes to n will be visible to the caller. }
procedure Increase(var n: integer; delta: integer);
begin
  writeln('Inside Increase: n = ', n, ', delta = ', delta);
  n := n + delta;
  writeln('Inside Increase after modification: n = ', n);
end;

{ Test procedure demonstrates both pass-by-value and pass-by-reference.
  The parameter refVal is passed by reference. }
procedure Test(inVal: integer; var refVal: integer);
begin
  writeln('Inside Test: inVal = ', inVal, ', refVal = ', refVal);
  refVal := refVal * 2;
  writeln('Inside Test after modifying refVal: refVal = ', refVal);
end;

{ The function Double takes a variable parameter (passed by reference)
  doubles its value, and returns the new value. }
function Double(var n: integer): integer;
begin
  writeln('Inside Double: n = ', n);
  n := n * 2;
  writeln('Inside Double after modification: n = ', n);
  Double := n;
end;

begin
  { Initialize variables }
  x := 10;
  y := 20;
  
  writeln('Before calling Increase: x = ', x);
  Increase(x, 5);   { x is passed by reference; its new value should be visible }
  writeln('After calling Increase: x = ', x);
  
  writeln;
  writeln('Before calling Test: x = ', x, ', y = ', y);
  Test(x, y);       { y is passed by reference; changes inside Test should persist }
  writeln('After calling Test: x = ', x, ', y = ', y);

  writeln;
  writeln('Before calling Double: x = ', x);
  writeln('Result of Double: ', Double(x));
  writeln('After calling Double: x = ', x);
end.

