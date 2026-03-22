program InterfaceVirtualSlotOrder;

type
  IWorker = interface
    procedure Describe;
  end;

  PWorker = ^TWorker;
  TWorker = record
    value: Integer;
    procedure SetValue(v: Integer);
    procedure Init;
    procedure Describe; virtual;
  end;

procedure TWorker.SetValue(v: Integer);
var
  selfPtr: PWorker;
begin
  selfPtr := myself;
  selfPtr^.value := v;
end;

procedure TWorker.Init;
var
  selfPtr: PWorker;
begin
  selfPtr := myself;
  selfPtr^.value := selfPtr^.value + 1;
end;

procedure TWorker.Describe;
var
  selfPtr: PWorker;
begin
  selfPtr := myself;
  writeln('PASS: interface describe=', selfPtr^.value);
end;

var
  worker: PWorker;
  iface: IWorker;
begin
  new(worker);
  worker^.value := 12;
  iface := IWorker(worker);
  iface.Describe;
end.
