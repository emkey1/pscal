program NestedDynamicArrayCommaIndex;

type
  IntGrid = array of array of Integer;

var
  grid: IntGrid;
  r, c: Integer;

begin
  SetLength(grid, 2);
  for r := 0 to 1 do
    SetLength(grid[r], 3);

  r := 1;
  c := 2;
  grid[r, c] := 42;

  if grid[r, c] = 42 then
    Writeln('PASS: nested dynamic array comma index')
  else
    Writeln('FAIL: nested dynamic array comma index');
end.
