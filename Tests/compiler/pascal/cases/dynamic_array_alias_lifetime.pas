program TestDynamicArrayAliasLifetime;

type
  TGrid = array of array of Integer;
  THolder = record
    Matrix: TGrid;
  end;

var
  holder: THolder;
  aliasGrid, replacement: TGrid;
begin
  SetLength(holder.Matrix, 3, 4);
  holder.Matrix[2][3] := 77;
  aliasGrid := holder.Matrix;

  SetLength(replacement, 2, 2);
  replacement[1][1] := 11;
  holder.Matrix := replacement;

  if Length(aliasGrid) <> 3 then writeln('FAIL: alias outer lifetime');
  if Length(aliasGrid[0]) <> 4 then writeln('FAIL: alias inner lifetime');
  if aliasGrid[2][3] <> 77 then writeln('FAIL: alias value lifetime');
  if Length(holder.Matrix) <> 2 then writeln('FAIL: replacement outer lifetime');
  if Length(holder.Matrix[0]) <> 2 then writeln('FAIL: replacement inner lifetime');
  if holder.Matrix[1][1] <> 11 then writeln('FAIL: replacement value lifetime');

  writeln('PASS: dynamic array alias lifetime');
end.
