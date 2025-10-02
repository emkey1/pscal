program TypeLocalShadow;
type
  TPair = record
    Left: Integer;
    Right: Integer;
  end;

procedure UseGlobal;
var
  pair: TPair;
begin
  pair.Left := 1;
  pair.Right := 2;
  writeln('global_pair=', pair.Left + pair.Right);
end;

procedure UseLocal;
type
  TPair = record
    Left: Integer;
    Right: Integer;
    Sum: Integer;
  end;
var
  pair: TPair;
begin
  pair.Left := 3;
  pair.Right := 4;
  pair.Sum := pair.Left + pair.Right;
  writeln('local_sum=', pair.Sum);
end;

begin
  UseLocal;
  UseGlobal;
end.