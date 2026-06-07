program Test_Interface_Assertions;

type
  IEmptyInterface = interface
    procedure Signal;
  end;

  TFirstRecord = record
    markerA: Integer;
    procedure Signal; virtual;
  end;

  TSecondRecord = record
    markerB: Integer;
    procedure Signal; virtual;
  end;

procedure TFirstRecord.Signal; begin myself^.markerA := 1; end;
procedure TSecondRecord.Signal; begin myself^.markerB := 2; end;

procedure RunTests;
var
  iface: IEmptyInterface;
  recA: ^TFirstRecord;
  recB: ^TSecondRecord;
  extractedA: ^TFirstRecord;
begin
  new(recA); recA^.markerA := 99;
  new(recB); recB^.markerB := 88;

  iface := IEmptyInterface(recA);

  if not (iface is TFirstRecord) then writeln('FAIL: Interface identity "is" recognition');
  if iface is TSecondRecord then writeln('FAIL: False type false-positive matching');

  extractedA := iface as TFirstRecord;
  if extractedA^.markerA <> 99 then writeln('FAIL: Extracted reference state target value mismatch');

  iface := IEmptyInterface(recB);
  if not (iface is TSecondRecord) then writeln('FAIL: Alternative interface "is" validation');

  dispose(recA);
  dispose(recB);
  writeln('SUITE 7 COMPLETE: Interface Assertions');
end;

begin
  RunTests;
end.
