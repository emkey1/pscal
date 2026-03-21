program RecordConstructorNamedFields;

type
  Direction = (LeftDir, RightDir, StayDir);

  MoveCommand = record
    direction: Direction;
  end;

  TransitionRule = record
    currentState: Byte;
    readSymbol: Byte;
    writeSymbol: Byte;
    moveCmd: MoveCommand;
    newState: Byte;
  end;

var
  rule: TransitionRule;

begin
  rule := (currentState: 1;
           readSymbol: 0;
           writeSymbol: 1;
           moveCmd: (direction: RightDir);
           newState: 2);

  if (rule.currentState = 1) and
     (rule.readSymbol = 0) and
     (rule.writeSymbol = 1) and
     (rule.moveCmd.direction = RightDir) and
     (rule.newState = 2) then
    Writeln('PASS: record constructor named fields')
  else
    Writeln('FAIL: record constructor named fields');
end.
