program AnonymousFunctionLiteralClosure;

type
  TAdder = function(delta: Integer): Integer;

function MakeAdder(base: Integer): TAdder;
begin
  Result := function(delta: Integer): Integer;
  begin
    Result := base + delta;
  end;
end;

procedure UseAdder(labelText: string; adder: TAdder);
begin
  Writeln(labelText, adder(3));
end;

var
  f: TAdder;

procedure Demo;
begin
  f := MakeAdder(10);
  Writeln('first=', f(5));
  UseAdder('inline=', function(delta: Integer): Integer;
                      begin
                        Result := delta * 2;
                      end);
end;

begin
  Demo;
end.
