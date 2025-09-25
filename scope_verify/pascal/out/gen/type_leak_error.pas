program TypeLeak;

procedure Factory;
type
  TInternal = record
    Value: Integer;
  end;
var
  item: TInternal;
begin
  item.Value := 1;
  writeln('inside=', item.Value);
end;

var
  other: TInternal;
begin
  Factory;
  other.Value := 2;
  writeln(other.Value);
end.