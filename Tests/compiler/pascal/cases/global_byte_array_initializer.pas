program GlobalByteArrayInitializer;

type
  State = array[0..8] of Byte;

var
  goalState: State = (1,2,3,4,5,6,7,8,0);

begin
  Writeln('PASS: global byte array = ',
          goalState[0], ' ',
          goalState[1], ' ',
          goalState[2], ' ',
          goalState[8]);
end.
