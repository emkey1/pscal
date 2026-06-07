program TestInterfaceArrayResultNestedIndex;

type
  TGrid = array of array of Integer;

  IGridEngine = interface
    procedure Setup(w, h: Integer);
    function GetGrid: TGrid;
  end;

  TAutomataEngine = record
    Width, Height: Integer;
    Matrix: TGrid;
    procedure Setup(w, h: Integer); virtual;
    function GetGrid: TGrid; virtual;
  end;

procedure TAutomataEngine.Setup(w, h: Integer);
var
  eng: ^TAutomataEngine;
begin
  eng := myself;
  eng^.Width := w;
  eng^.Height := h;
  SetLength(eng^.Matrix, h, w);
  eng^.Matrix[2][2] := 7;
end;

function TAutomataEngine.GetGrid: TGrid;
var
  eng: ^TAutomataEngine;
begin
  eng := myself;
  GetGrid := eng^.Matrix;
end;

function ExtractEngineGrid(const engine: IGridEngine): TGrid;
begin
  ExtractEngineGrid := engine.GetGrid();
end;

var
  rawEngine: ^TAutomataEngine;
  gridInterface: IGridEngine;
  centerCell: Integer;
begin
  new(rawEngine);
  gridInterface := IGridEngine(rawEngine);
  gridInterface.Setup(6, 6);

  centerCell := ExtractEngineGrid(gridInterface)[2][2];
  writeln('PASS: interface array result nested index ', centerCell);

  dispose(rawEngine);
end.
