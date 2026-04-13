program SubrangeTypeAlias;

type
  TEdgeIdx = 0..11;

var
  idx: TEdgeIdx;

begin
  idx := 3;
  Writeln('PASS: subrange type alias idx=', idx);
end.
