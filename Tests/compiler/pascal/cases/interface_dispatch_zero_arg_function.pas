program Test_Interface_Dispatch;

type
  ICompTarget = interface
    procedure Execute(factor: Integer);
    function FetchMetric: Integer;
  end;

  TConcreteWorker = record
    idLabel: string;
    internalAccumulator: Integer;
    procedure Execute(factor: Integer); virtual;
    function FetchMetric: Integer; virtual;
  end;

procedure TConcreteWorker.Execute(factor: Integer);
var
  instance: ^TConcreteWorker;
begin
  instance := myself;
  instance^.internalAccumulator += (factor * 2);
end;

function TConcreteWorker.FetchMetric: Integer;
var
  instance: ^TConcreteWorker;
begin
  instance := myself;
  FetchMetric := instance^.internalAccumulator;
end;

procedure RunTests;
var
  worker: ^TConcreteWorker;
  targetInterface: ICompTarget;
begin
  new(worker);
  worker^.idLabel := 'WorkerAlpha';
  worker^.internalAccumulator := 100;

  targetInterface := ICompTarget(worker);
  targetInterface.Execute(5);
  targetInterface.Execute(10);

  if targetInterface.FetchMetric() <> 130 then
    writeln('FAIL: Interface dispatch or myself receiver addressing error');

  dispose(worker);
  writeln('SUITE 6 COMPLETE: Interface Dispatch');
end;

begin
  RunTests;
end.
