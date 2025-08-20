program ConstArrayTest;

const
  CardValues: array[1..3] of string = ('A','2','3');
  IntSeq: array[0..2] of integer = (10,-5,42);
  TestStr: string = 'Simple';

var
  i: integer;
  s: string;

begin
  writeln('Simple Const TestStr: ', TestStr);
  writeln('CardValues[1] = ', CardValues[1]);
  writeln('CardValues[3] = ', CardValues[3]);
  s := CardValues[2];
  writeln('Assigned CardValues[2] to s: ', s);
  for i := 0 to 2 do
    write(IntSeq[i], ' ');
  writeln;
end.
