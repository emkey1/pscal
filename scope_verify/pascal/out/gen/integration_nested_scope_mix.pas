program IntegrationScopeMix;
const
  Factor = 2;
type
  TInfo = record
    Value: Integer;
  end;

procedure Compute(start: Integer);
const
  Factor = 3;
type
  TInfo = record
    Value: Integer;
    Total: Integer;
  end;
var
  info: TInfo;
  step: Integer;

  procedure AddStep(multiplier: Integer);
  begin
    info.Total := info.Total + multiplier * start;
  end;

begin
  info.Value := start;
  info.Total := 0;
  for step := 1 to Factor do
  begin
    AddStep(step);
  end;
  writeln('inner_total=', info.Total);
  writeln('local_factor=', Factor);
end;

var
  summary: TInfo;
begin
  summary.Value := 4;
  Compute(2);
  writeln('outer_scaled=', summary.Value * Factor);
end.