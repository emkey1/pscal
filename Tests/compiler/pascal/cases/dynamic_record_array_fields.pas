program DynamicRecordArrayFields;

type
  Node = record
    value: Integer;
  end;

var
  items: array of Node;

begin
  SetLength(items, 1);
  items[0].value := 42;
  Writeln('PASS: dynamic record array fields = ', items[0].value);
end.
