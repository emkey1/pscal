program TestPointers;

type
  PInt = ^Integer; // Declare a pointer type to Integer
  PRec = ^TRec;    // Declare a pointer type to a Record
  TRec = record
    id: Integer;
    name: String[20];
  end;

var
  p1, p2 : PInt;    // Declare integer pointer variables
  recPtr : PRec;   // Declare a record pointer variable

begin
  writeln('--- Integer Pointer Test ---');

  // 1. Initialization and Nil Check
  p1 := nil;
  if p1 = nil then
    writeln('p1 is initially nil (Correct)')
  else
    writeln('p1 is NOT initially nil (Incorrect)');

  // 2. Allocation with new()
  new(p1);
  if p1 = nil then
    writeln('p1 is nil after new() (Incorrect)')
  else
    writeln('p1 is not nil after new() (Correct)');

  // 3. Assignment via Dereference
  p1^ := 42;
  writeln('Assigned 42 to p1^');

  // 4. Reading via Dereference
  writeln('Value of p1^: ', p1^);

  // 5. Pointer Assignment
  p2 := p1;
  writeln('Assigned p1 to p2');
  writeln('Value of p2^: ', p2^);

  // 6. Modifying through one pointer affects the other
  p2^ := 99;
  writeln('Assigned 99 to p2^');
  writeln('Value of p1^ is now: ', p1^);

  // 7. Disposal
  writeln('Disposing p1...');
  dispose(p1);
  // After dispose, accessing p1^ or p2^ is undefined behavior,
  // but p1 and p2 themselves should ideally be nil or point to invalid memory.
  // Standard Pascal doesn't mandate setting the variable to nil, but our implementation does.
  // We can check if the variable itself was set to nil internally.
  // Note: A robust test might require specific runtime checks or debugging.
  if p1 = nil then
     writeln('p1 is nil after dispose(p1) (Implementation Specific Check - Correct for ours)')
  else
     writeln('p1 is NOT nil after dispose(p1) (Implementation Specific Check - Might be ok)');

  // Dispose p2 (points to same memory, should ideally handle double dispose gracefully or error)
  // Standard Pascal makes double dispose an error. Our current implementation might crash or error.
  // writeln('Attempting to dispose p2 (already disposed memory)...');
  // dispose(p2); // Commented out - likely runtime error

  writeln(''); // Blank line

  writeln('--- Record Pointer Test ---');
  recPtr := nil;
  if recPtr = nil then writeln('recPtr is initially nil (Correct)');

  new(recPtr);
  if recPtr <> nil then writeln('recPtr is not nil after new() (Correct)');

  // Assign values using dereference and field access
  recPtr^.id := 101;
  recPtr^.name := 'Test Record';
  writeln('Assigned values to recPtr^ fields.');

  // Read values back
  writeln('recPtr^.id = ', recPtr^.id);
  writeln('recPtr^.name = ', recPtr^.name);

  // Dispose the record
  writeln('Disposing recPtr...');
  dispose(recPtr);
  if recPtr = nil then writeln('recPtr is nil after dispose (Correct for ours)');

  writeln('');
  writeln('Pointer tests complete.');
end.
