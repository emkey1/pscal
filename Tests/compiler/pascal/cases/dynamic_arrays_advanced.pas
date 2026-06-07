program Test_Dynamic_Arrays_Advanced;

type
  TMatrix = array of array of Integer;
  TLinear = array of Integer;

function BuildSequence(size: Integer): TLinear;
var
  res: TLinear;
begin
  SetLength(res, size);
  for var i := 0 to size - 1 do res[i] := i * 10;
  BuildSequence := res;
end;

procedure RunTests;
var
  matrix: TMatrix;
  primary, aliasRef: TLinear;
begin
  SetLength(matrix, 3, 4);
  if Length(matrix) <> 3 then writeln('FAIL: Multidimensional Y-axis sizing');
  if Length(matrix[0]) <> 4 then writeln('FAIL: Multidimensional X-axis sizing');

  matrix[2][3] := 888;
  if matrix[2][3] <> 888 then writeln('FAIL: Multi-dimensional matrix block slot assignment');

  SetLength(primary, 3);
  primary[0] := 55;
  aliasRef := primary;

  aliasRef[0] := 99;
  if primary[0] <> 99 then writeln('FAIL: Reference tracking sync modification fail via reference alias');

  if BuildSequence(5)[3] <> 30 then
    writeln('FAIL: Direct functional selector evaluation break on active results');

  writeln('SUITE 9 COMPLETE: Dynamic Arrays Advanced');
end;

begin
  RunTests;
end.
