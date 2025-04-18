program EnumTest;

type
  TMyEnum = (valOne, valTwo, valThree); // Simple enum type

var
  myVar: TMyEnum;       // Variable of the enum type
  anotherVar: integer; // Another variable to ensure basic assignments work

begin
  anotherVar := 123;     // Test basic assignment
  writeln('Basic assignment OK. AnotherVar = ', anotherVar);

  writeln('Attempting enum assignment...');
  myVar := valTwo;       // <<< This is the critical assignment to test

  // If it gets past the assignment, print the ordinal
  writeln('Enum assignment OK. myVar ordinal = ', ord(myVar));

end.
