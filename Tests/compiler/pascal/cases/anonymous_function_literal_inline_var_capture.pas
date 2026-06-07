program AnonymousFunctionLiteralInlineVarCapture;

type
  TTransform = function(delta: Integer): Integer;

var
  observed: Integer;

procedure RunIt;
var
  myAdder: TTransform;
begin
  var offset: Integer;
  offset := 500;

  myAdder := function(delta: Integer): Integer;
  begin
    Result := offset + delta;
  end;

  observed := myAdder(5);
  Writeln('PASS: ', observed);
end;

begin
  RunIt;
end.
