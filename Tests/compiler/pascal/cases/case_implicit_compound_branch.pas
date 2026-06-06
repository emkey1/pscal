program CaseImplicitCompoundBranch;

type
  TDir = (North, South, West, East);
  TPos = record
    R, C: Integer;
  end;

var
  d: TDir;
  cur, nxt: TPos;

begin
  cur.R := 10;
  cur.C := 20;
  d := North;

  case d of
    North: nxt := cur; nxt.R := cur.R - 2;
    South: begin nxt := cur; nxt.R := cur.R + 2; end;
    West : nxt := cur; nxt.C := cur.C - 2;
    East : nxt := cur; nxt.C := cur.C + 2;
  end;

  Writeln('PASS: ', nxt.R, ',', nxt.C);
end.
