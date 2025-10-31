program InterfaceSelfReturn;

type
  IFoo = interface
    function Clone: IFoo;
  end;

var
  Stub: IFoo;

begin
  Stub := nil;
  if Stub = nil then
    writeln('interface returns itself works');
end.
