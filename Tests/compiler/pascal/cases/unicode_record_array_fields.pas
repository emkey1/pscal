program UnicodeRecordArrayFields;

type
  TNode = record
    Name: UnicodeString;
    Mark: WideChar;
  end;

var
  Nodes: array[1..2] of TNode;
begin
  Nodes[1].Name := '雪山';
  Nodes[1].Mark := '雪';
  Nodes[2].Name := Nodes[1].Name + '→';
  Nodes[2].Mark := Nodes[2].Name[3];

  Writeln('name1=', Nodes[1].Name);
  Writeln('mark1=', Nodes[1].Mark);
  Writeln('name2=', Nodes[2].Name);
  Writeln('mark2=', Nodes[2].Mark);
end.
